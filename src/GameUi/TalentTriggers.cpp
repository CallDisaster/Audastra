#include "War3MapCore/GameUi/TalentTriggers.h"

#include "FeatureTextWriter.h"
#include "UiBindingCheck.h"
#include "War3MapCore/GameUi/UiTriggers.h"

#include <cctype>
#include <set>
#include <tuple>
#include <utility>

// 统一天赋树的自然 T。下标固定按上限展开，层数、列数改了已有登记也不错位：
// - 登记：英雄槽 h、层 t、列 o → (h−1)×28 + (t−1)×4 + o；
// - 同步单位：玩家 p、层 t、列 o → (p−1)×28 + (t−1)×4 + o；
// - 选择结果：玩家 p、层 t → p×8 + t。
// 同步部分（绑定、计算、选择）只在所有玩家相同的事件里改；本机部分（刷新、入口、点选、悬停）只改界面数值，
// 不创建句柄，用自己的 AUT_L* 临时变量。
namespace War3Map::GameUi {
namespace {

using namespace detail;

constexpr std::string_view kKey = "Talent";
constexpr std::uint32_t kHeroStride = kTalentMaxTiers * kTalentMaxOptions;
constexpr std::uint32_t kPickStride = kTalentMaxTiers + 1;
constexpr std::string_view kDim = "|cff808080";

std::string V(std::string_view name) { return "udg_AUT_" + std::string(name); }
std::string At(std::string_view name, std::string_view index) { return V(name) + "[" + std::string(index) + "]"; }
std::string Add(std::string_view a, std::string_view b) { return Int(a, "OperatorAdd", b); }
std::string Sub(std::string_view a, std::string_view b) { return Int(a, "OperatorSubtract", b); }
std::string Mul(std::string_view a, std::string_view b) { return Int(a, "OperatorMultiply", b); }
std::string Concat(std::string_view a, std::string_view b) { return Call("OperatorString", {std::string(a), std::string(b)}); }
std::string SameUnit(std::string_view a, std::string_view b) {
    return Call("OperatorCompareUnit", {std::string(a), "OperatorEqualENE", std::string(b)});
}
std::string Equal(std::string_view a, std::string_view b) { return CompareInt(a, "OperatorEqual", b); }
std::string NotEmpty(std::string_view text) {
    return Call("OperatorCompareString", {std::string(text), "OperatorNotEqualENE", Quote("")});
}
std::string Comment(std::string_view text) { return Call("CommentString", {Quote(text)}); }

// (组 − 1) × 28 + (层 − 1) × 4 + 列
std::string Slot(std::string_view group, std::string_view tier, std::string_view option) {
    return Add(Add(Mul(Sub(group, "1"), std::to_string(kHeroStride)), Mul(Sub(tier, "1"), std::to_string(kTalentMaxOptions))), option);
}
std::string Picked(std::string_view player, std::string_view tier) {
    return At("Picked", Add(Mul(player, std::to_string(kPickStride)), tier));
}
std::uint32_t RegisteredSlot(std::uint32_t hero, std::uint32_t tier, std::uint32_t option) {
    return (hero - 1) * kHeroStride + (tier - 1) * kTalentMaxOptions + option;
}

std::string PlayerConstant(std::uint32_t index) { return std::string("Player") + (index < 10 ? "0" : "") + std::to_string(index); }

class Generator {
public:
    explicit Generator(const TalentSystemOptions& options) : o_(options), b_(options.frame) {}

    Result<TalentSystemPlan> Run() {
        Check();
        if (!errors_.empty()) {
            Result<TalentSystemPlan> failure;
            failure.diagnostics = std::move(errors_);
            return failure;
        }
        Variables();
        // 被运行的触发器写在前面。
        if (o_.includeRegistration) text_.Trigger(Name("登记", "Register"), {}, Registration());
        text_.Trigger(Name("计算", "Count"), {}, Count());
        text_.Trigger(Name("刷新", "Refresh"), {}, Refresh());
        text_.Trigger(Name("绑定", "Bind"), {}, Bind());
        text_.Trigger(Name("候选", "Candidate"), {}, Candidate());
        text_.Trigger(Name("初始化", "Init"), {}, Init(), {}, "init");
        text_.Trigger(Name("英雄进入", "HeroEnter"), {Call("TriggerRegisterEnterRectSimple", {"GetPlayableMapRect()"})},
            {Set(V("Unit"), "GetTriggerUnit()"), RunTrigger("候选", "Candidate")});
        text_.Trigger(Name("升级", "Level"), {Call("TriggerRegisterAnyUnitEventBJ", {"PlayerUnitEventHero_Level"})}, Level());
        Lines selection;
        for (std::uint32_t p = 0; p < o_.players; ++p) {
            selection.push_back(Call("TriggerRegisterPlayerSelectionEventBJ", {PlayerConstant(p), "SelDeselOptionSel"}));
        }
        text_.Trigger(Name("选择", "Pick"), selection, Pick(),
            {Call("OperatorCompareUnitCode", {Call("GetUnitTypeId", {"GetTriggerUnit()"}), "OperatorEqualENE", "'" + o_.syncUnitType + "'"})});
        if (!b_.open.empty()) text_.Trigger(Name("入口", "Open"), {OnUi(b_.open)}, Open());
        if (!b_.close.empty()) text_.Trigger(Name("关闭", "Close"), {OnUi(b_.close)}, Close());
        for (std::size_t c = 0; c < b_.columns.size(); ++c) {
            const std::string column = std::to_string(c + 1);
            text_.Trigger(Name("点选" + column, "Click" + column), {OnUi(b_.columns[c].pick)}, Click(c));
            if (!b_.columns[c].enter.empty())
                text_.Trigger(Name("移入" + column, "Enter" + column), {OnUi(b_.columns[c].enter)}, Enter(c));
            if (!b_.columns[c].leave.empty())
                text_.Trigger(Name("移出" + column, "Leave" + column), {OnUi(b_.columns[c].leave)}, Leave());
        }

        TalentSystemPlan plan;
        plan.featureText = text_.Text();
        plan.variables = std::move(text_.variables);
        plan.triggers = std::move(text_.triggers);
        plan.tiers = tiers_;
        plan.options = options_;
        plan.pickedVariable = "AUT_Picked";
        plan.bindTrigger = Name("绑定", "Bind");
        plan.registrationTrigger = Name("登记", "Register");
        return Result<TalentSystemPlan>::Success(std::move(plan));
    }

private:
    [[nodiscard]] const UiDescription& d() const { return *b_.description; }
    void Error(const char* code, const std::string& message) { errors_.push_back(Note(DiagnosticSeverity::Error, code, message)); }

    [[nodiscard]] std::string Name(std::string_view role, std::string_view key) const { return TalentTriggerName(o_.name, role, key); }
    [[nodiscard]] std::string RunTrigger(std::string_view role, std::string_view key) const {
        return Call("ConditionalTriggerExecute", {TriggerRef(Name(role, key))});
    }
    [[nodiscard]] std::string OnUi(const std::string& event) const {
        return Call("AudastraUITriggerRegisterEvent", {Quote(UiEventKey(d().id, event))});
    }
    [[nodiscard]] std::string UiUpdate() const {
        return Call("ConditionalTriggerExecute", {TriggerRef(UiTriggerName(d(), "更新", "Update"))});
    }
    [[nodiscard]] std::string Value(const std::string& id) const { return "udg_" + UiValueVariableName(d().id, id); }
    [[nodiscard]] std::string Cell(const std::string& id, std::string_view row) const { return Value(id) + "[" + std::string(row) + "]"; }
    // 可空的数值：没绑定时不写。
    void SetIf(Lines& out, const std::string& id, const std::string& value) const {
        if (!id.empty()) out.push_back(Set(Value(id), value));
    }

    void Check() {
        if (!UsableLabel(o_.name)) Error("AUT_OPTION_INVALID", "天赋系统的名字不能为空，也不能含双引号或控制字符");
        if (!ValidRawcode(o_.syncUnitType)) Error("AUT_OPTION_INVALID", "同步单位类型应为 4 个字符的物编编号");
        if (o_.players < 1 || o_.players > kTalentMaxPlayers)
            Error("AUT_OPTION_INVALID", "玩家数应在 1 到 " + std::to_string(kTalentMaxPlayers) + " 之间");
        if (!o_.afterPick.empty() && !UsableLabel(o_.afterPick)) Error("AUT_OPTION_INVALID", "选定后运行的触发器名不能含双引号或控制字符");
        CheckFrame();
        if (tiers_ == 0) return;
        if (!o_.unlockLevels.empty() && o_.unlockLevels.size() != tiers_) {
            Error("AUT_OPTION_INVALID", "解锁等级有 " + std::to_string(o_.unlockLevels.size()) + " 个，界面有 " + std::to_string(tiers_) + " 层");
        }
        for (const auto level : o_.unlockLevels) {
            if (level < 1 || level > 10000) Error("AUT_OPTION_INVALID", "解锁等级应在 1 到 10000 之间");
        }
        CheckHeroes();
    }

    void CheckFrame() {
        if (!b_.description) {
            Error("AUT_FRAME_INVALID", "天赋树没有界面描述");
            return;
        }
        const auto single = [&](const std::string& id, UiValueType type, const char* what, bool required) {
            if (id.empty()) {
                if (required) Error("AUT_FRAME_INVALID", std::string("天赋树需要") + what + "数值");
                return;
            }
            if (const auto problem = CheckValueBinding(d(), id, type, 0, what)) Error("AUT_FRAME_INVALID", *problem);
        };
        single(b_.shown, UiValueType::Boolean, "布尔", true);
        single(b_.entryShown, UiValueType::Boolean, "布尔", false);
        single(b_.entry, UiValueType::String, "字符串", false);
        single(b_.title, UiValueType::String, "字符串", false);
        single(b_.points, UiValueType::String, "字符串", false);
        single(b_.tipShown, UiValueType::Boolean, "布尔", false);
        single(b_.tipTitle, UiValueType::String, "字符串", false);
        single(b_.tipText, UiValueType::String, "字符串", false);
        if (b_.tipShown.empty() && (!b_.tipTitle.empty() || !b_.tipText.empty()))
            Error("AUT_FRAME_INVALID", "绑定了说明框的标题或正文，需要同时绑定说明框显示");

        const UiValue* level = b_.level.empty() ? nullptr : FindValue(d(), b_.level);
        if (!level || level->type != UiValueType::String || level->size == 0 || level->size > kTalentMaxTiers) {
            Error("AUT_FRAME_INVALID", level ? "界面 " + d().id + " 的数值 " + b_.level + " 应为字符串数组，1 到 " + std::to_string(kTalentMaxTiers) + " 格"
                                             : "天赋树需要每层等级的字符串数组数值");
            return;
        }
        tiers_ = level->size;
        if (b_.columns.empty() || b_.columns.size() > kTalentMaxOptions) {
            Error("AUT_FRAME_INVALID", "每层选项应为 1 到 " + std::to_string(kTalentMaxOptions) + " 列");
            return;
        }
        options_ = static_cast<std::uint32_t>(b_.columns.size());
        std::set<std::string> events;
        const auto event = [&](const std::string& id, UiEventKind kind, bool repeated, bool required) {
            if (id.empty()) {
                if (required) Error("AUT_FRAME_INVALID", "每列选项需要点击事件");
                return;
            }
            if (!events.insert(id).second) Error("AUT_FRAME_INVALID", "事件 " + id + " 绑定了两次");
            const auto* found = FindEvent(d(), id);
            if (!found) {
                Error("AUT_FRAME_INVALID", "界面 " + d().id + " 没有事件 " + id);
                return;
            }
            if (found->kind != kind) {
                const char* names[] = {"点击", "移入", "移出"};
                Error("AUT_FRAME_INVALID", "事件 " + id + " 应为" + names[static_cast<int>(kind)] + "事件");
                return;
            }
            if (!repeated) return;
            bool exists = false;
            const UiFrame* owner = RepeatedOwner(d().frames, found->frame, nullptr, exists);
            if (!owner || owner->repeat->count != tiers_) {
                Error("AUT_FRAME_INVALID", "事件 " + id + " 的框架 " + found->frame + " 应在重复 " + std::to_string(tiers_) + " 次的框架里");
            }
        };
        event(b_.open, UiEventKind::Click, false, false);
        event(b_.close, UiEventKind::Click, false, false);
        for (const auto& column : b_.columns) {
            for (const auto& [id, type, what] : {std::tuple{column.icon, UiValueType::String, "字符串"},
                     std::tuple{column.name, UiValueType::String, "字符串"}, std::tuple{column.picked, UiValueType::Boolean, "布尔"}}) {
                if (id.empty()) Error("AUT_FRAME_INVALID", "每列选项需要图标、名字和已选三个数组数值");
                else if (const auto problem = CheckValueBinding(d(), id, type, tiers_, what)) Error("AUT_FRAME_INVALID", *problem);
            }
            event(column.pick, UiEventKind::Click, true, true);
            if (!column.enter.empty() || !column.leave.empty()) {
                if (b_.tipShown.empty()) Error("AUT_FRAME_INVALID", "绑定了移入移出事件，需要说明框显示数值");
            }
            event(column.enter, UiEventKind::Enter, true, false);
            event(column.leave, UiEventKind::Leave, true, false);
        }
    }

    void CheckHeroes() {
        if (o_.heroes.size() > kTalentMaxHeroes) Error("AUT_ENTRY_INVALID", "最多登记 " + std::to_string(kTalentMaxHeroes) + " 个英雄");
        for (std::size_t h = 0; h < o_.heroes.size(); ++h) {
            const TalentHero& hero = o_.heroes[h];
            const std::string at = "第 " + std::to_string(h + 1) + " 个英雄（" + hero.unitType + "）：";
            if (!ValidRawcode(hero.unitType)) Error("AUT_ENTRY_INVALID", at + "英雄类型应为 4 个字符的物编编号");
            if (!hero.label.empty() && !UsableLabel(hero.label)) Error("AUT_ENTRY_INVALID", at + "注释不能含双引号或控制字符");
            for (std::size_t j = 0; j < h; ++j) {
                if (o_.heroes[j].unitType == hero.unitType) Error("AUT_ENTRY_INVALID", at + "已在第 " + std::to_string(j + 1) + " 个登记");
            }
            std::set<std::pair<std::uint32_t, std::uint32_t>> seen;
            for (const TalentEntry& entry : hero.entries) {
                const std::string where = at + "第 " + std::to_string(entry.tier) + " 层第 " + std::to_string(entry.option) + " 项";
                if (entry.tier < 1 || entry.tier > tiers_ || entry.option < 1 || entry.option > options_) {
                    Error("AUT_ENTRY_INVALID", where + "超出界面的 " + std::to_string(tiers_) + " 层 × " + std::to_string(options_) + " 列");
                    continue;
                }
                if (!seen.insert({entry.tier, entry.option}).second) Error("AUT_ENTRY_INVALID", where + "登记了两次");
                if (!UsableLabel(entry.name)) Error("AUT_ENTRY_INVALID", where + "需要名字，名字不能含双引号或控制字符");
                if (!UsableLabel(entry.icon)) Error("AUT_ENTRY_INVALID", where + "需要图标路径");
                if (!entry.disabledIcon.empty() && !UsableLabel(entry.disabledIcon)) Error("AUT_ENTRY_INVALID", where + "灰色图标路径无效");
                if (!entry.learn.empty() && !ValidRawcode(entry.learn)) Error("AUT_ENTRY_INVALID", where + "学会的技能应为 4 个字符的物编编号");
            }
        }
    }

    void Variables() {
        const auto var = [&](std::string_view name, std::string_view type, bool array = false, const std::string& initial = {}) {
            text_.Variable("AUT_" + std::string(name), type, array ? std::optional<std::uint32_t>(0) : std::nullopt, initial);
        };
        // 登记
        var("HeroCount", "integer", false, "0");
        var("HeroType", "unitcode", true);
        var("Name", "string", true);
        var("Icon", "string", true);
        var("IconOff", "string", true);
        var("Text", "string", true);
        var("Learn", "abilcode", true);
        var("Learns", "boolean", true);
        var("TierLevel", "integer", true);
        // 同步数据：每个玩家一份
        var("Hero", "unit", true);
        var("Group", "integer", true);
        var("Open", "integer", true);
        var("Free", "integer", true);
        var("Picked", "integer", true);
        var("Sync", "unit", true);
        // 绑定的输入与选定后的输出
        var("Unit", "unit");
        var("PickUnit", "unit");
        var("PickTier", "integer", false, "0");
        var("PickOption", "integer", false, "0");
        // 同步部分的临时变量
        for (const auto* name : {"P", "T", "O", "K", "I", "J", "Found"}) var(name, "integer", false, "0");
        var("Point", "location");
        // 本机部分的临时变量
        for (const auto* name : {"LP", "LT", "LK", "LRow", "LLevel", "LPick"}) var(name, "integer", false, "0");
    }

    Lines Registration() const {
        Lines out{Comment("每个英雄类型一组：先写英雄类型，再按 层、列 写名字、图标、灰色图标、说明和学会的技能（不学技能时不写）"),
            Comment("下标 = (英雄序号 - 1) x " + std::to_string(kHeroStride) + " + (层 - 1) x " + std::to_string(kTalentMaxOptions) + " + 列"),
            Set(V("HeroCount"), std::to_string(o_.heroes.size()))};
        for (std::size_t h = 0; h < o_.heroes.size(); ++h) {
            const TalentHero& hero = o_.heroes[h];
            const auto index = static_cast<std::uint32_t>(h + 1);
            out.push_back(Comment(hero.label.empty() ? hero.unitType : hero.label));
            out.push_back(Set(At("HeroType", std::to_string(index)), "'" + hero.unitType + "'"));
            for (const TalentEntry& entry : hero.entries) {
                const std::string slot = std::to_string(RegisteredSlot(index, entry.tier, entry.option));
                out.push_back(Comment("第 " + std::to_string(entry.tier) + " 层第 " + std::to_string(entry.option) + " 项：" + entry.name));
                out.push_back(Set(At("Name", slot), Quote(entry.name)));
                out.push_back(Set(At("Icon", slot), Quote(entry.icon)));
                out.push_back(Set(At("IconOff", slot), Quote(entry.disabledIcon.empty() ? TalentDisabledIcon(entry.icon) : entry.disabledIcon)));
                out.push_back(Set(At("Text", slot), Quote(entry.text)));
                if (!entry.learn.empty()) {
                    out.push_back(Set(At("Learn", slot), "'" + entry.learn + "'"));
                    out.push_back(Set(At("Learns", slot), "true"));
                }
            }
        }
        return out;
    }

    [[nodiscard]] std::string HeroLevel(std::string_view player) const { return Call("GetHeroLevel", {At("Hero", player)}); }

    // 玩家 AUT_P 的已开放层数与可选数。
    Lines Count() const {
        const std::string p = V("P");
        Lines out{Comment("先设 AUT_P 为玩家编号；可选数 = 已开放、尚未选择的层数"), Set(At("Open", p), "0"), Set(At("Free", p), "0")};
        Lines tier = If({CompareInt(HeroLevel(p), "OperatorGreaterEq", At("TierLevel", V("I")))},
            {Set(At("Open", p), Add(At("Open", p), "1"))});
        Append(tier, If({CompareInt(HeroLevel(p), "OperatorGreaterEq", At("TierLevel", V("I"))), Equal(Picked(p, V("I")), "0")},
            {Set(At("Free", p), Add(At("Free", p), "1"))}));
        Append(out, If({CompareInt(At("Group", p), "OperatorGreater", "0")}, ForLoopVar(V("I"), "1", std::to_string(tiers_), tier)));
        return out;
    }

    // 用户玩家（正在游戏）的英雄，不含幻象，该玩家还没有英雄。
    Lines Candidate() const {
        const std::string owner = Call("GetOwningPlayer", {V("Unit")});
        return If({IsTrue(Call("IsUnitType", {V("Unit"), "UnitTypeHero"})),
                      Call("OperatorCompareBoolean", {Call("IsUnitIllusionBJ", {V("Unit")}), "OperatorEqualENE", "false"}),
                      Call("OperatorComparePlayerControl", {Call("GetPlayerController", {owner}), "OperatorEqualENE", "MapControlUser"}),
                      Call("OperatorComparePlayerSlotStatus", {Call("GetPlayerSlotState", {owner}), "OperatorEqualENE", "PlayerSlotStatePlaying"}),
                      Equal(At("Group", Call("GetConvertedPlayerId", {owner})), "0")},
            {RunTrigger("绑定", "Bind")});
    }

    Lines Bind() const {
        const std::string p = V("P");
        Lines out{Comment("先设 AUT_Unit 为英雄；类型登记过时成为其所属玩家的天赋英雄（换英雄时已选结果保留）"),
            Set(V("Found"), "0")};
        Append(out, ForLoopVar(V("I"), "1", V("HeroCount"),
            If({Call("OperatorCompareUnitCode", {Call("GetUnitTypeId", {V("Unit")}), "OperatorEqualENE", At("HeroType", V("I"))})},
                {Set(V("Found"), V("I"))})));
        Append(out, If({CompareInt(V("Found"), "OperatorGreater", "0")},
            {Set(p, Call("GetConvertedPlayerId", {Call("GetOwningPlayer", {V("Unit")})})), Set(At("Hero", p), V("Unit")),
                Set(At("Group", p), V("Found")), RunTrigger("计算", "Count"), RunTrigger("刷新", "Refresh")}));
        return out;
    }

    Lines Init() const {
        const std::string player = Call("ConvertedPlayer", {V("P")});
        // 登记不带事件，由这里先运行：开局绑定英雄前登记一定已写好。
        Lines out{RunTrigger("登记", "Register")};
        for (std::uint32_t t = 1; t <= tiers_; ++t) {
            const std::uint32_t level = o_.unlockLevels.empty() ? t * 5 : o_.unlockLevels[t - 1];
            out.push_back(Set(At("TierLevel", std::to_string(t)), std::to_string(level)));
        }
        out.push_back(Comment("同步单位：每个用户玩家每个选项一个，透明、无敌、暂停、不占人口，放在地图左下角之外"));
        const std::string corner = Call("GetEntireMapRect");
        out.push_back(Set(V("Point"), Call("Location", {Arith(Call("GetRectMinX", {corner}), "OperatorAdd", "64"),
                                                     Arith(Call("GetRectMinY", {corner}), "OperatorAdd", "64")})));
        const std::string unit = "GetLastCreatedUnit()";
        Lines create{Call("CreateNUnitsAtLoc", {"1", "'" + o_.syncUnitType + "'", player, V("Point"), "0"}),
            Set(At("Sync", Slot(V("P"), V("T"), V("O"))), unit), Call("SetUnitUseFoodBJ", {"EnableDisableDisable", unit}),
            Call("SetUnitInvulnerable", {unit, "InvulnerabilityInvulnerable"}), Call("SetUnitPathing", {unit, "PathingOff"}),
            // 创建时游戏会把单位挪到最近的可通行处（可玩区域里），关掉碰撞后再放回区域外，玩家框选不到。
            Call("SetUnitPositionLoc", {unit, V("Point")}),
            Call("SetUnitVertexColorBJ", {unit, "100", "100", "100", "100"}), Call("PauseUnitBJ", {"PauseUnpauseOptionPause", unit})};
        Append(out, ForLoopVar(V("P"), "1", std::to_string(o_.players),
            If({Call("OperatorComparePlayerControl", {Call("GetPlayerController", {player}), "OperatorEqualENE", "MapControlUser"}),
                   Call("OperatorComparePlayerSlotStatus", {Call("GetPlayerSlotState", {player}), "OperatorEqualENE", "PlayerSlotStatePlaying"})},
                ForLoopVar(V("T"), "1", std::to_string(tiers_), ForLoopVar(V("O"), "1", std::to_string(options_), create)))));
        out.push_back(Call("RemoveLocation", {V("Point")}));
        Append(out, Block(Call("ForGroupMultiple", {Call("GetUnitsInRectAll", {"GetPlayableMapRect()"})}), "actions",
            {Set(V("Unit"), "GetEnumUnit()"), RunTrigger("候选", "Candidate")}));
        out.push_back(RunTrigger("刷新", "Refresh"));
        return out;
    }

    Lines Level() const {
        const std::string p = V("P");
        Lines out{Set(p, Call("GetConvertedPlayerId", {Call("GetOwningPlayer", {"GetTriggerUnit()"})}))};
        Append(out, If({SameUnit("GetTriggerUnit()", At("Hero", p))}, {RunTrigger("计算", "Count"), RunTrigger("刷新", "Refresh")}));
        return out;
    }

    // 所有玩家都收到的选择单位事件：认出同步单位对应的层与列，校验后写入结果。
    Lines Pick() const {
        const std::string p = V("P");
        const std::string slot = Slot(At("Group", p), V("T"), V("O"));
        Lines out{Comment("同步单位被选中：所有玩家都会运行，这里写入同步的选择结果"),
            Set(p, Call("GetConvertedPlayerId", {"GetTriggerPlayer()"})), Set(V("T"), "0"), Set(V("O"), "0")};
        Append(out, ForLoopVar(V("I"), "1", std::to_string(tiers_), ForLoopVar(V("J"), "1", std::to_string(options_),
            If({SameUnit("GetTriggerUnit()", At("Sync", Slot(p, V("I"), V("J"))))}, {Set(V("T"), V("I")), Set(V("O"), V("J"))}))));
        Lines accept{Set(V("K"), slot), Set(Picked(p, V("T")), V("O"))};
        Append(accept, If({IsTrue(At("Learns", V("K")))}, {Call("UnitAddAbilityBJ", {At("Learn", V("K")), At("Hero", p)})}));
        accept.push_back(RunTrigger("计算", "Count"));
        accept.push_back(Set(V("PickUnit"), At("Hero", p)));
        accept.push_back(Set(V("PickTier"), V("T")));
        accept.push_back(Set(V("PickOption"), V("O")));
        if (!o_.afterPick.empty()) accept.push_back(Call("ConditionalTriggerExecute", {TriggerRef(o_.afterPick)}));
        accept.push_back(RunTrigger("刷新", "Refresh"));
        Lines found{Call("SelectUnitRemoveForPlayer", {"GetTriggerUnit()", "GetTriggerPlayer()"})};
        Append(found, If({CompareInt(At("Group", p), "OperatorGreater", "0"),
                             CompareInt(HeroLevel(p), "OperatorGreaterEq", At("TierLevel", V("T"))), Equal(Picked(p, V("T")), "0"),
                             NotEmpty(At("Name", slot))},
            accept));
        Append(out, If({CompareInt(V("T"), "OperatorGreater", "0")}, found));
        return out;
    }

    // 本机玩家的界面数值。
    Lines Refresh() const {
        const std::string lp = V("LP");
        const std::string row = V("LRow");
        const std::string level = At("TierLevel", V("LT"));
        const std::string opened = CompareInt(V("LLevel"), "OperatorGreaterEq", level);
        Lines tier{Set(row, Sub(V("LT"), "1")), Set(V("LPick"), Picked(lp, V("LT")))};
        Append(tier, If({opened}, {Set(Cell(b_.level, row), Concat(Call("I2S", {level}), Quote(" 级")))},
            {Set(Cell(b_.level, row), Concat(Concat(Quote(std::string(kDim) + "需要 "), Call("I2S", {level})), Quote(" 级|r")))}));
        for (std::size_t c = 0; c < b_.columns.size(); ++c) {
            const TalentColumnBinding& column = b_.columns[c];
            const std::string option = std::to_string(c + 1);
            tier.push_back(Set(V("LK"), Slot(At("Group", lp), V("LT"), option)));
            Append(tier, If({Equal(V("LPick"), option)}, {Set(Cell(column.picked, row), "true")}, {Set(Cell(column.picked, row), "false")}));
            // 已开放且（这层未选或选的就是这一项）时正常显示，否则变灰。
            const Lines bright{Set(Cell(column.icon, row), At("Icon", V("LK"))), Set(Cell(column.name, row), At("Name", V("LK")))};
            Lines dim{Set(Cell(column.icon, row), At("IconOff", V("LK"))), Set(Cell(column.name, row), Quote(""))};
            Append(dim, If({NotEmpty(At("Name", V("LK")))},
                {Set(Cell(column.name, row), Concat(Concat(Quote(kDim), At("Name", V("LK"))), Quote("|r")))}));
            Append(tier, If({opened, Equal(V("LPick"), "0")}, bright, If({opened, Equal(V("LPick"), option)}, bright, dim)));
        }

        Lines bound;
        SetIf(bound, b_.entryShown, "true");
        bound.push_back(Set(V("LLevel"), HeroLevel(lp)));
        if (!b_.entry.empty() || !b_.points.empty()) {
            Lines some, none;
            SetIf(some, b_.entry, Concat(Concat(Quote(o_.name + " ("), Call("I2S", {At("Free", lp)})), Quote(")")));
            SetIf(some, b_.points, Concat(Quote("可选 "), Call("I2S", {At("Free", lp)})));
            SetIf(none, b_.entry, Quote(o_.name));
            SetIf(none, b_.points, Quote(""));
            Append(bound, If({CompareInt(At("Free", lp), "OperatorGreater", "0")}, some, none));
        }
        SetIf(bound, b_.title, Concat(Quote(o_.name + " · "), Call("GetHeroProperName", {At("Hero", lp)})));
        Append(bound, ForLoopVar(V("LT"), "1", std::to_string(tiers_), tier));

        Lines unbound;
        SetIf(unbound, b_.entryShown, "false");
        unbound.push_back(Set(Value(b_.shown), "false"));
        SetIf(unbound, b_.tipShown, "false");

        Lines out{Comment("只改本机界面：内容是本机玩家英雄的那一份"), Set(lp, Call("GetConvertedPlayerId", {"GetLocalPlayer()"}))};
        Append(out, If({CompareInt(At("Group", lp), "OperatorGreater", "0")}, bound, unbound));
        out.push_back(UiUpdate());
        return out;
    }

    Lines Open() const {
        Lines hide{Set(Value(b_.shown), "false")};
        SetIf(hide, b_.tipShown, "false");
        Lines out{Comment("界面点击只在本机发生：只开关本机面板")};
        Append(out, If({IsTrue(Value(b_.shown))}, hide, {Set(Value(b_.shown), "true")}));
        out.push_back(RunTrigger("刷新", "Refresh"));
        return out;
    }

    Lines Close() const {
        Lines out{Set(Value(b_.shown), "false")};
        SetIf(out, b_.tipShown, "false");
        out.push_back(UiUpdate());
        return out;
    }

    // 本机点了第 行 层第 c 列：把对应的同步单位加进本机选择，由游戏同步给所有玩家。
    Lines Click(std::size_t c) const {
        const std::string lp = V("LP");
        const std::string option = std::to_string(c + 1);
        Lines out{Comment("界面点击只在本机发生：选中同步单位，由“选择”触发器在所有玩家上写入结果"),
            Set(lp, Call("GetConvertedPlayerId", {"GetLocalPlayer()"})), Set(V("LT"), Add("AudastraUIGetTriggerIndex()", "1")),
            Set(V("LK"), Slot(At("Group", lp), V("LT"), option))};
        Append(out, If({CompareInt(At("Group", lp), "OperatorGreater", "0"), CompareInt(V("LT"), "OperatorLessEq", std::to_string(tiers_)),
                           CompareInt(HeroLevel(lp), "OperatorGreaterEq", At("TierLevel", V("LT"))), Equal(Picked(lp, V("LT")), "0"),
                           NotEmpty(At("Name", V("LK")))},
            {Call("SelectUnitAddForPlayer", {At("Sync", Slot(lp, V("LT"), option)), "GetLocalPlayer()"})}));
        return out;
    }

    Lines Enter(std::size_t c) const {
        const std::string lp = V("LP");
        const std::string option = std::to_string(c + 1);
        Lines show{Set(Value(b_.tipShown), "true")};
        SetIf(show, b_.tipTitle, At("Name", V("LK")));
        SetIf(show, b_.tipText,
            Concat(Concat(Concat(Concat(Quote("第 "), Call("I2S", {V("LT")})), Quote(" 层 · ")), Call("I2S", {At("TierLevel", V("LT"))})),
                Concat(Quote(" 级|n"), At("Text", V("LK")))));
        show.push_back(UiUpdate());
        Lines out{Set(lp, Call("GetConvertedPlayerId", {"GetLocalPlayer()"})), Set(V("LT"), Add("AudastraUIGetTriggerIndex()", "1")),
            Set(V("LK"), Slot(At("Group", lp), V("LT"), option))};
        Append(out, If({CompareInt(At("Group", lp), "OperatorGreater", "0"), NotEmpty(At("Name", V("LK")))}, show));
        return out;
    }

    Lines Leave() const { return {Set(Value(b_.tipShown), "false"), UiUpdate()}; }

    const TalentSystemOptions& o_;
    const TalentFrameBinding& b_;
    std::uint32_t tiers_ = 0;
    std::uint32_t options_ = 0;
    FeatureTextBuilder text_;
    std::vector<Diagnostic> errors_;
};

} // namespace

Result<TalentSystemPlan> GenerateTalentSystem(const TalentSystemOptions& options) {
    return Generator(options).Run();
}

std::string TalentTriggerName(std::string_view name, std::string_view role, std::string_view key) {
    return std::string(name) + " " + std::string(role) + " " + std::string(kKey) + "_" + std::string(key);
}

std::string TalentDisabledIcon(std::string_view icon) {
    const auto lower = [](std::string_view text) {
        std::string out(text);
        for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    };
    const std::string folded = lower(icon);
    constexpr std::string_view kFolder = "commandbuttons\\btn";
    const auto at = folded.find(kFolder);
    if (at == std::string::npos) return std::string(icon);
    const std::string_view keep = icon.substr(0, at);
    const std::string_view rest = icon.substr(at + kFolder.size());
    return std::string(keep) + "CommandButtonsDisabled\\DISBTN" + std::string(rest);
}

} // namespace War3Map::GameUi
