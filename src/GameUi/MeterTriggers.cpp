#include "War3MapCore/GameUi/MeterTriggers.h"

#include "FeatureTextWriter.h"
#include "UiBindingCheck.h"
#include "War3MapCore/GameUi/UiTriggers.h"

// 伤害统计的自然 T。每个玩家的伤害在 AUM_<键>_Amount[玩家编号]（1 起），由伤害事件累加（同步数据）。刷新每 0.5 秒
// 按伤害从高到低选出前几名写进界面数值：所有玩家做同样的运算，界面调用只影响各自的画面，不用本地玩家。
namespace War3Map::GameUi {
namespace {

using namespace detail;

constexpr std::size_t kMaxKeyLength = 24;

class Generator {
public:
    explicit Generator(const MeterSystemOptions& options) : o_(options), b_(options.frame) {}

    Result<MeterSystemPlan> Run() {
        Check();
        if (!errors_.empty()) {
            Result<MeterSystemPlan> failure;
            failure.diagnostics = std::move(errors_);
            return failure;
        }
        Variables();
        text_.Trigger(Name("记录", "Record"), {Call("YDWESyStemAnyUnitDamagedRegistTrigger")}, Damage());
        text_.Trigger(Name("刷新", "Refresh"), {Call("TriggerRegisterTimerEventPeriodic", {Real(kMeterRefreshSeconds)})}, Refresh());
        text_.Trigger(Name("清零", "Reset"), {}, Reset());

        MeterSystemPlan plan;
        plan.featureText = text_.Text();
        plan.variables = std::move(text_.variables);
        plan.triggers = std::move(text_.triggers);
        plan.rows = rows_;
        plan.resetTrigger = Name("清零", "Reset");
        plan.amountVariable = Var("Amount");
        return Result<MeterSystemPlan>::Success(std::move(plan));
    }

private:
    [[nodiscard]] const UiDescription& d() const { return *b_.description; }
    void Error(const char* code, const std::string& message) { errors_.push_back(Note(DiagnosticSeverity::Error, code, message)); }

    [[nodiscard]] std::string Var(std::string_view name) const { return "AUM_" + o_.key + "_" + std::string(name); }
    [[nodiscard]] std::string V(std::string_view name) const { return "udg_" + Var(name); }
    [[nodiscard]] std::string At(std::string_view name, std::string_view index) const { return V(name) + "[" + std::string(index) + "]"; }
    [[nodiscard]] std::string Name(std::string_view role, std::string_view roleKey) const {
        return MeterTriggerName(o_.name, o_.key, role, roleKey);
    }
    [[nodiscard]] std::string Value(const std::string& id) const { return "udg_" + UiValueVariableName(d().id, id); }

    void Check() {
        if (!UsableLabel(o_.name)) Error("AUM_OPTION_INVALID", "伤害统计的名字不能为空，也不能含双引号或控制字符");
        if (!IsValidUiId(o_.key) || o_.key.size() > kMaxKeyLength)
            Error("AUM_OPTION_INVALID", "伤害统计的键应为 ASCII 字母或下划线开头的标识符，最长 " + std::to_string(kMaxKeyLength));
        if (o_.players < 1 || o_.players > kMeterMaxPlayers)
            Error("AUM_OPTION_INVALID", "玩家数应在 1 到 " + std::to_string(kMeterMaxPlayers) + " 之间");
        if (!b_.description) {
            Error("AUM_FRAME_INVALID", "伤害统计没有界面描述");
            return;
        }
        if (b_.shown.empty()) {
            Error("AUM_FRAME_INVALID", "伤害统计需要“显示”布尔数组");
            return;
        }
        const UiValue* shown = FindValue(d(), b_.shown);
        if (!shown || shown->type != UiValueType::Boolean || shown->size < 1 || shown->size > kMeterMaxRows) {
            Error("AUM_FRAME_INVALID", "界面 " + d().id + " 的数值 " + b_.shown + " 应为布尔数组（1 到 " + std::to_string(kMeterMaxRows) + " 格），格数就是行数");
            return;
        }
        rows_ = shown->size;
        const auto bind = [&](const std::string& id, UiValueType type, std::uint32_t size, std::string_view what) {
            if (id.empty()) return;
            if (const auto problem = CheckValueBinding(d(), id, type, size, what)) Error("AUM_FRAME_INVALID", *problem);
        };
        bind(b_.name, UiValueType::String, rows_, "文字");
        bind(b_.amount, UiValueType::Real, rows_, "实数");
        bind(b_.amountText, UiValueType::String, rows_, "文字");
        bind(b_.share, UiValueType::Real, rows_, "实数");
        bind(b_.percent, UiValueType::Real, rows_, "实数");
        bind(b_.rate, UiValueType::Real, rows_, "实数");
        bind(b_.color, UiValueType::String, rows_, "文字");
        bind(b_.total, UiValueType::Real, 0, "实数");
        bind(b_.time, UiValueType::Real, 0, "实数");
    }

    void Variables() {
        text_.Variable(Var("Amount"), "real", 0, "0");
        text_.Variable(Var("Picked"), "boolean", 0, "false");
        text_.Variable(Var("Source"), "unit");
        text_.Variable(Var("P"), "integer", std::nullopt, "0");
        text_.Variable(Var("R"), "integer", std::nullopt, "0");
        text_.Variable(Var("Best"), "integer", std::nullopt, "0");
        text_.Variable(Var("Total"), "real", std::nullopt, "0");
        text_.Variable(Var("Top"), "real", std::nullopt, "0");
        text_.Variable(Var("Time"), "real", std::nullopt, "0");
        text_.Variable(Var("Running"), "boolean", std::nullopt, "false");
    }

    Lines Damage() const {
        const std::string owner = Call("GetOwningPlayer", {V("Source")});
        Lines conditions{CompareReal("GetEventDamage()", "OperatorGreater", "0"),
            Call("OperatorCompareUnit", {V("Source"), "OperatorNotEqualENE", "UnitNull"})};
        if (o_.enemiesOnly) conditions.push_back(IsTrue(Call("IsUnitEnemy", {"GetTriggerUnit()", owner})));
        conditions.push_back(CompareInt(Call("GetConvertedPlayerId", {owner}), "OperatorLessEq", std::to_string(o_.players)));
        Lines out{Call("CommentString", {Quote("伤害来源单位的所有者记为造成伤害的玩家")}), Set(V("Source"), "GetEventDamageSource()")};
        Append(out, If(conditions, {Set(V("P"), Call("GetConvertedPlayerId", {owner})),
                                       Set(At("Amount", V("P")), Arith(At("Amount", V("P")), "OperatorAdd", "GetEventDamage()")),
                                       Set(V("Running"), "true")}));
        return out;
    }

    Lines Refresh() const {
        const std::string players = std::to_string(o_.players);
        Lines out = If({IsTrue(V("Running"))}, {Set(V("Time"), Arith(V("Time"), "OperatorAdd", Real(kMeterRefreshSeconds)))});
        out.push_back(Set(V("Total"), "0"));
        Append(out, ForLoopVar(V("P"), "1", players,
            {Set(At("Picked", V("P")), "false"), Set(V("Total"), Arith(V("Total"), "OperatorAdd", At("Amount", V("P"))))}));

        // 每行选出还没排上的玩家里伤害最高的一个（同样多时编号小的在前）。
        const std::string amount = At("Amount", V("P"));
        const std::string best = At("Amount", V("Best"));
        const Lines pick = If({CompareInt(V("Best"), "OperatorEqual", "0")}, {Set(V("Best"), V("P"))},
            If({CompareReal(amount, "OperatorGreater", best)}, {Set(V("Best"), V("P"))}));
        Lines row{Set(V("Best"), "0")};
        Append(row, ForLoopVar(V("P"), "1", players,
            If({Call("OperatorCompareBoolean", {At("Picked", V("P")), "OperatorEqualENE", "false"}), CompareReal(amount, "OperatorGreater", "0")}, pick)));

        const std::string cell = Int(V("R"), "OperatorSubtract", "1");
        const auto at = [&](const std::string& id) { return Value(id) + "[" + cell + "]"; };
        Lines filled{Set(At("Picked", V("Best")), "true")};
        Append(filled, If({CompareInt(V("R"), "OperatorEqual", "1")}, {Set(V("Top"), best)}));
        filled.push_back(Set(at(b_.shown), "true"));
        if (!b_.name.empty()) filled.push_back(Set(at(b_.name), Call("GetPlayerName", {Call("ConvertedPlayer", {V("Best")})})));
        if (!b_.amount.empty()) filled.push_back(Set(at(b_.amount), best));
        if (!b_.amountText.empty()) {
            Append(filled, If({CompareReal(best, "OperatorGreaterEq", "10000")},
                {Set(at(b_.amountText), Call("OperatorString", {Call("AudastraUIFormatReal", {Arith(best, "OperatorDivide", "10000"), "1"}), Quote("万")}))},
                {Set(at(b_.amountText), Call("AudastraUIFormatReal", {best, "0"}))}));
        }
        if (!b_.share.empty()) filled.push_back(Set(at(b_.share), Arith(best, "OperatorDivide", V("Top"))));
        if (!b_.percent.empty()) filled.push_back(Set(at(b_.percent), Arith(Arith(best, "OperatorDivide", V("Total")), "OperatorMultiply", "100")));
        if (!b_.rate.empty()) filled.push_back(Set(at(b_.rate), Arith(best, "OperatorDivide", Call("RMaxBJ", {V("Time"), "1"}))));
        if (!b_.color.empty()) {
            // 队伍颜色贴图按玩家编号：玩家 1 为 TeamColor00。
            const std::string index = Call("I2S", {Int(V("Best"), "OperatorSubtract", "1")});
            const auto path = [&](const std::string& number) {
                return Call("OperatorString", {Call("OperatorString", {Quote("ReplaceableTextures\\TeamColor\\TeamColor"), number}), Quote(".blp")});
            };
            Append(filled, If({CompareInt(V("Best"), "OperatorLessEq", "10")}, {Set(at(b_.color), path(Call("OperatorString", {Quote("0"), index})))},
                {Set(at(b_.color), path(index))}));
        }
        Append(row, If({CompareInt(V("Best"), "OperatorGreater", "0")}, filled, {Set(at(b_.shown), "false")}));
        Append(out, ForLoopVar(V("R"), "1", std::to_string(rows_), row));
        if (!b_.total.empty()) out.push_back(Set(Value(b_.total), V("Total")));
        if (!b_.time.empty()) out.push_back(Set(Value(b_.time), V("Time")));
        out.push_back(Call("ConditionalTriggerExecute", {TriggerRef(UiTriggerName(d(), "更新", "Update"))}));
        return out;
    }

    Lines Reset() const {
        Lines out{Call("CommentString", {Quote("把所有玩家的伤害与秒数清零；秒数从清零后第一次记到伤害开始")})};
        Append(out, ForLoopVar(V("P"), "1", std::to_string(o_.players), {Set(At("Amount", V("P")), "0")}));
        out.push_back(Set(V("Running"), "false"));
        out.push_back(Set(V("Time"), "0"));
        return out;
    }

    const MeterSystemOptions& o_;
    const MeterFrameBinding& b_;
    std::uint32_t rows_ = 0;
    FeatureTextBuilder text_;
    std::vector<Diagnostic> errors_;
};

} // namespace

Result<MeterSystemPlan> GenerateMeterSystem(const MeterSystemOptions& options) {
    return Generator(options).Run();
}

std::string MeterTriggerName(std::string_view name, std::string_view key, std::string_view role, std::string_view roleKey) {
    return std::string(name) + " " + std::string(role) + " " + std::string(key) + "_" + std::string(roleKey);
}

} // namespace War3Map::GameUi
