#include "War3MapCore/GameUi/TargetTriggers.h"

#include "FeatureTextWriter.h"
#include "UiBindingCheck.h"
#include "UnitDisplayLines.h"
#include "UnitSourceLines.h"
#include "War3MapCore/GameUi/UiTriggers.h"

// 目标框架的自然 T。每个玩家的目标在 AUF_<键>_Unit[玩家编号]（1 起）。刷新先做所有玩家相同的同步部分（清掉已移除的单位），
// 再取本机玩家的目标写界面数值：本机部分只做数值运算和界面调用，不创建句柄。
namespace War3Map::GameUi {
namespace {

using namespace detail;

constexpr std::size_t kMaxKeyLength = 24;

UnitDisplayFields DisplayFields(const TargetFrameBinding& b) {
    return UnitDisplayFields{b.health, b.mana, b.healthNow, b.healthMax, b.manaNow, b.manaMax, b.name, b.dead, b.portrait, b.buffIcons};
}

class Generator {
public:
    explicit Generator(const TargetSystemOptions& options)
        : o_(options), b_(options.frame), source_("AUF_" + options.key, options.source, options.players) {}

    Result<TargetSystemPlan> Run() {
        Check();
        if (!errors_.empty()) {
            Result<TargetSystemPlan> failure;
            failure.diagnostics = std::move(errors_);
            return failure;
        }
        Variables();
        if (display_->Buffs() && o_.includeRegistration) text_.Trigger(Name("登记", "Register"), {}, display_->BuffRegistration(), {}, "init");
        if (display_->Portraits() && o_.includePortraitRegistration)
            text_.Trigger(Name("头像", "Portrait"), {}, display_->PortraitRegistration(), {}, "init");
        source_.Triggers(text_, [this](std::string_view role, std::string_view roleKey) { return Name(role, roleKey); });
        text_.Trigger(Name("刷新", "Refresh"), {Call("TriggerRegisterTimerEventPeriodic", {Real(kTargetRefreshSeconds)})}, Refresh());
        if (!b_.select.empty()) {
            text_.Trigger(Name("选中", "Select"), {Call("AudastraUITriggerRegisterEvent", {Quote(UiEventKey(d().id, b_.select))})}, Select());
        }

        TargetSystemPlan plan;
        plan.featureText = text_.Text();
        plan.variables = std::move(text_.variables);
        plan.triggers = std::move(text_.triggers);
        if (o_.source == TargetSource::Focus) {
            plan.memberVariable = Var("Member");
            plan.playerVariable = Var("Player");
            plan.setTrigger = Name("设定", "Set");
        }
        if (display_->Buffs()) plan.registrationTrigger = Name("登记", "Register");
        if (display_->Portraits()) plan.portraitTrigger = Name("头像", "Portrait");
        return Result<TargetSystemPlan>::Success(std::move(plan));
    }

private:
    [[nodiscard]] const UiDescription& d() const { return *b_.description; }
    void Error(const char* code, const std::string& message) { errors_.push_back(Note(DiagnosticSeverity::Error, code, message)); }

    [[nodiscard]] std::string Var(std::string_view name) const { return source_.Var(name); }
    [[nodiscard]] std::string V(std::string_view name) const { return source_.V(name); }
    // 该玩家的目标。
    [[nodiscard]] std::string Mine(std::string_view player) const { return source_.Mine(player); }
    [[nodiscard]] std::string Name(std::string_view role, std::string_view roleKey) const {
        return TargetTriggerName(o_.name, o_.key, role, roleKey);
    }
    [[nodiscard]] std::string Value(const std::string& id) const { return "udg_" + UiValueVariableName(d().id, id); }

    void Check() {
        if (!UsableLabel(o_.name)) Error("AUF_OPTION_INVALID", "目标框架的名字不能为空，也不能含双引号或控制字符");
        if (!IsValidUiId(o_.key) || o_.key.size() > kMaxKeyLength)
            Error("AUF_OPTION_INVALID", "目标框架的键应为 ASCII 字母或下划线开头的标识符，最长 " + std::to_string(kMaxKeyLength));
        if (o_.players < 1 || o_.players > kTargetMaxPlayers)
            Error("AUF_OPTION_INVALID", "玩家数应在 1 到 " + std::to_string(kTargetMaxPlayers) + " 之间");
        if (!b_.description) {
            Error("AUF_FRAME_INVALID", "目标框架没有界面描述");
            return;
        }
        if (b_.shown.empty()) {
            Error("AUF_FRAME_INVALID", "目标框架需要“显示”布尔数值");
            return;
        }
        if (const auto problem = CheckValueBinding(d(), b_.shown, UiValueType::Boolean, 0, "布尔")) {
            Error("AUF_FRAME_INVALID", *problem);
            return;
        }
        display_.emplace("AUF_" + o_.key, "AUF", d(), DisplayFields(b_), 0, o_.nameSource, o_.buffs, o_.portraits);
        display_->Check(errors_);
        if (!b_.level.empty()) {
            if (const auto problem = CheckValueBinding(d(), b_.level, UiValueType::Integer, 0, "整数")) Error("AUF_FRAME_INVALID", *problem);
        }
        if (!b_.select.empty()) CheckSelect();
    }

    void CheckSelect() {
        const UiEvent* event = FindEvent(d(), b_.select);
        if (!event) {
            Error("AUF_FRAME_INVALID", "界面 " + d().id + " 没有事件 " + b_.select);
            return;
        }
        if (event->kind != UiEventKind::Click) {
            Error("AUF_FRAME_INVALID", "选中目标要用点击事件，事件 " + b_.select + " 不是点击");
            return;
        }
        bool found = false;
        if (RepeatedOwner(d().frames, event->frame, nullptr, found)) {
            Error("AUF_FRAME_INVALID", "事件 " + b_.select + " 的框架 " + event->frame + " 在重复框架里；目标框架只有一个单位");
        }
    }

    void Variables() {
        source_.Variables(text_);
        text_.Variable(Var("P"), "integer", std::nullopt, "0");
        text_.Variable(Var("Self"), "unit");
        if (!b_.select.empty()) text_.Variable(Var("ClickResult"), "integer", std::nullopt, "0");
        display_->Variables(text_);
    }

    Lines Refresh() const {
        Lines out = source_.Cleanup(V("P"));
        out.push_back(Call("CommentString", {Quote("以下只改本机界面")}));
        out.push_back(Set(V("Self"), Mine("GetLocalPlayer()")));
        const auto at = [&](const std::string& id) { return Value(id); };
        Lines shown{Set(at(b_.shown), "true")};
        Append(shown, display_->Fill(V("Self"), at));
        if (!b_.level.empty()) {
            Append(shown, If({IsTrue(Call("IsUnitType", {V("Self"), "UnitTypeHero"}))}, {Set(at(b_.level), Call("GetHeroLevel", {V("Self")}))},
                {Set(at(b_.level), Call("GetUnitLevel", {V("Self")}))}));
        }
        Append(out, If({Call("OperatorCompareUnit", {V("Self"), "OperatorNotEqualENE", "UnitNull"})}, shown, {Set(at(b_.shown), "false")}));
        out.push_back(Call("ConditionalTriggerExecute", {TriggerRef(UiTriggerName(d(), "更新", "Update"))}));
        return out;
    }

    // 本机举着单位目标技能时把点击交给游戏作为目标（结果大于 0），否则（含运行库不支持）只改本机的选择。
    Lines Select() const {
        Lines click{Set(V("ClickResult"), Call("AudastraUIClickUnit", {V("Self")}))};
        Append(click, If({CompareInt(V("ClickResult"), "OperatorLessEq", "0")}, {Call("SelectUnitForPlayerSingle", {V("Self"), "GetLocalPlayer()"})}));
        Lines out{Call("CommentString", {Quote("界面点击只在本机发生：举着单位目标技能时作为目标交给游戏，否则只改本机的选择")}),
            Set(V("Self"), Mine("GetLocalPlayer()"))};
        Append(out, If({Call("OperatorCompareUnit", {V("Self"), "OperatorNotEqualENE", "UnitNull"})}, click));
        return out;
    }

    const TargetSystemOptions& o_;
    const TargetFrameBinding& b_;
    UnitSourceLines source_;
    std::optional<UnitDisplayLines> display_;
    FeatureTextBuilder text_;
    std::vector<Diagnostic> errors_;
};

} // namespace

Result<TargetSystemPlan> GenerateTargetSystem(const TargetSystemOptions& options) {
    return Generator(options).Run();
}

std::string TargetTriggerName(std::string_view name, std::string_view key, std::string_view role, std::string_view roleKey) {
    return std::string(name) + " " + std::string(role) + " " + std::string(key) + "_" + std::string(roleKey);
}

} // namespace War3Map::GameUi
