#pragma once

#include "FeatureTextWriter.h"
#include "War3MapCore/GameUi/TargetTriggers.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

// 目标框架与技能栏共用：每个玩家一个“当前单位”，存在 <前缀>_Unit[玩家编号]（1 起，同步数据）。
// 来源为“选中”时玩家选中单位（游戏同步的选择事件）就记下它、取消选择时清掉；来源为“焦点”时设好 <前缀>_Member、<前缀>_Player
// 后运行“设定”触发器（单位为空时清除）。刷新先清掉已移除的单位。
namespace War3Map::GameUi::detail {

class UnitSourceLines {
public:
    // prefix 为变量前缀（不带 udg_ 与结尾的下划线），players 为记录的玩家数。
    UnitSourceLines(std::string prefix, TargetSource source, std::uint32_t players)
        : prefix_(std::move(prefix)), source_(source), players_(players) {}

    [[nodiscard]] std::string Var(std::string_view name) const { return prefix_ + "_" + std::string(name); }
    [[nodiscard]] std::string V(std::string_view name) const { return "udg_" + Var(name); }
    // 该玩家的当前单位。
    [[nodiscard]] std::string Mine(std::string_view player) const {
        return V("Unit") + "[" + Call("GetConvertedPlayerId", {std::string(player)}) + "]";
    }
    // 编号为 index（整数写法）的玩家的当前单位。
    [[nodiscard]] std::string At(std::string_view index) const { return V("Unit") + "[" + std::string(index) + "]"; }

    void Variables(FeatureTextBuilder& text) const {
        text.Variable(Var("Unit"), "unit", 0);
        if (source_ == TargetSource::Focus) {
            text.Variable(Var("Member"), "unit");
            text.Variable(Var("Player"), "player");
        }
    }

    // name(用途, 用途键) 给出触发器名：选中为“选择”“取消选择”，焦点为“设定”。
    template <typename NameOf>
    void Triggers(FeatureTextBuilder& text, const NameOf& name) const {
        if (source_ == TargetSource::Selection) {
            text.Trigger(name("选择", "Pick"), SelectionEvents("SelDeselOptionSel"), {Set(Mine("GetTriggerPlayer()"), "GetTriggerUnit()")});
            text.Trigger(name("取消选择", "Unpick"), SelectionEvents("SelDeselOptionDesel"),
                If({Call("OperatorCompareUnit", {Mine("GetTriggerPlayer()"), "OperatorEqualENE", "GetTriggerUnit()"})},
                    {Set(Mine("GetTriggerPlayer()"), "UnitNull")}));
        } else {
            text.Trigger(name("设定", "Set"), {},
                {Call("CommentString", {Quote("先把单位设给 " + Var("Member") + "、玩家设给 " + Var("Player") + "；单位为空时清除该玩家的单位")}),
                    Set(Mine(V("Player")), V("Member"))});
        }
    }

    // 所有玩家相同的同步部分：用整数变量 index 循环，清掉已移除的单位（已移除的单位最大生命为 0）。
    [[nodiscard]] Lines Cleanup(const std::string& index) const {
        const std::string unit = At(index);
        return ForLoopVar(index, "1", std::to_string(players_),
            If({Call("OperatorCompareUnit", {unit, "OperatorNotEqualENE", "UnitNull"}),
                   CompareReal(Call("GetUnitStateSwap", {"UnitStateMaxLife", unit}), "OperatorLessEq", "0")},
                {Set(unit, "UnitNull")}));
    }

    [[nodiscard]] Lines SelectionEvents(std::string_view option) const {
        Lines events;
        for (std::uint32_t p = 0; p < players_; ++p) {
            events.push_back(Call("TriggerRegisterPlayerSelectionEventBJ",
                {std::string("Player") + (p < 10 ? "0" : "") + std::to_string(p), std::string(option)}));
        }
        return events;
    }

private:
    std::string prefix_;
    TargetSource source_;
    std::uint32_t players_;
};

} // namespace War3Map::GameUi::detail
