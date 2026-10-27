#pragma once

#include "FeatureTextWriter.h"
#include "UiBindingCheck.h"
#include "War3MapCore/GameUi/UnitDisplay.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// 团队框架与目标框架共用：检查单位显示的数值绑定、声明临时变量、写增减益与头像的登记，并把一个单位的生命、法力、
// 头像、名字、死亡、增减益填进绑定的界面数值。变量名与诊断码带各系统的前缀（团队框架为 AUP；目标框架变量为 AUF_<键>、诊断码为 AUF）。
namespace War3Map::GameUi::detail {

// 绑定的界面数值编号，空为不绑定；格数都与系统的显示数值相同（0 为单值）。
struct UnitDisplayFields {
    std::string health;     // 实数 0..1：生命比例
    std::string mana;       // 实数 0..1：法力比例（没有法力为 0）
    std::string healthNow;  // 实数：当前生命
    std::string healthMax;  // 实数：最大生命
    std::string manaNow;    // 实数：当前法力
    std::string manaMax;    // 实数：最大法力
    std::string name;       // 字符串
    std::string dead;       // 布尔：死亡
    std::string portrait;   // 字符串：单位类型的头像图标路径，没登记时为空
    std::vector<std::string> buffIcons;  // 字符串：第 1、2…个增减益的图标路径，没有时为空
};

class UnitDisplayLines {
public:
    // prefix 为变量前缀（不带 udg_ 与结尾的下划线），code 为诊断码前缀；size 为数值格数（0 为单值）。
    UnitDisplayLines(std::string prefix, std::string code, const UiDescription& description, UnitDisplayFields fields, std::uint32_t size,
        UnitNameSource nameSource, const std::vector<UnitBuff>& buffs, const std::vector<UnitPortrait>& portraits)
        : prefix_(std::move(prefix)), code_(std::move(code)), d_(description), f_(std::move(fields)), size_(size), nameSource_(nameSource), buffs_(buffs),
          portraits_(portraits) {}

    [[nodiscard]] bool Buffs() const { return !f_.buffIcons.empty(); }
    [[nodiscard]] bool Portraits() const { return !f_.portrait.empty(); }

    // 一次列出全部问题。
    void Check(std::vector<Diagnostic>& errors) const {
        const auto value = [&](const std::string& id, UiValueType type, const char* what) {
            if (id.empty()) return;
            if (const auto problem = CheckValueBinding(d_, id, type, size_, what)) Error(errors, "FRAME", *problem);
        };
        value(f_.health, UiValueType::Real, "实数");
        value(f_.mana, UiValueType::Real, "实数");
        value(f_.healthNow, UiValueType::Real, "实数");
        value(f_.healthMax, UiValueType::Real, "实数");
        value(f_.manaNow, UiValueType::Real, "实数");
        value(f_.manaMax, UiValueType::Real, "实数");
        value(f_.name, UiValueType::String, "字符串");
        value(f_.dead, UiValueType::Boolean, "布尔");
        value(f_.portrait, UiValueType::String, "字符串");
        CheckBuffs(errors);
        CheckPortraits(errors);
    }

    void Variables(FeatureTextBuilder& text) const {
        const auto var = [&](std::string_view name, std::string_view type, bool array = false, const std::string& initial = {}) {
            text.Variable(prefix_ + "_" + std::string(name), type, array ? std::optional<std::uint32_t>(0) : std::nullopt, initial);
        };
        if (Buffs()) {
            var("BuffCount", "integer", false, "0");
            var("BuffCode", "buffcode", true);
            var("BuffIcon", "string", true);
            var("BuffHit", "string", true);
            var("BuffN", "integer", false, "0");
            var("BuffI", "integer", false, "0");
        }
        if (Portraits()) {
            var("TypeCount", "integer", false, "0");
            var("TypeCode", "unitcode", true);
            var("TypeIcon", "string", true);
            var("TypeI", "integer", false, "0");
        }
    }

    [[nodiscard]] Lines BuffRegistration() const {
        Lines out{Call("CommentString", {Quote("每个增减益一组：增减益、图标路径；按这里的顺序显示单位身上有的增减益")}),
            Set(V("BuffCount"), std::to_string(buffs_.size()))};
        for (std::size_t i = 0; i < buffs_.size(); ++i) {
            const std::string index = std::to_string(i + 1);
            out.push_back(Set(At("BuffCode", index), "'" + buffs_[i].buff + "'"));
            out.push_back(Set(At("BuffIcon", index), Quote(buffs_[i].icon)));
        }
        return out;
    }

    [[nodiscard]] Lines PortraitRegistration() const {
        Lines out{Call("CommentString", {Quote("每种单位一组：单位类型、头像图标路径；没登记的单位头像为空")}),
            Set(V("TypeCount"), std::to_string(portraits_.size()))};
        for (std::size_t i = 0; i < portraits_.size(); ++i) {
            const std::string index = std::to_string(i + 1);
            out.push_back(Set(At("TypeCode", index), "'" + portraits_[i].unit + "'"));
            out.push_back(Set(At("TypeIcon", index), Quote(portraits_[i].icon)));
        }
        return out;
    }

    // 把 unit 的显示填进绑定的数值；at(编号) 为该数值这一格的写法。只做数值运算和界面数值赋值，可在本机部分运行。
    template <typename CellRef>
    [[nodiscard]] Lines Fill(const std::string& unit, const CellRef& at) const {
        Lines out;
        if (!f_.health.empty()) Append(out, Ratio(at(f_.health), "UnitStateLife", "UnitStateMaxLife", unit));
        if (!f_.mana.empty()) Append(out, Ratio(at(f_.mana), "UnitStateMana", "UnitStateMaxMana", unit));
        if (!f_.healthNow.empty()) out.push_back(Set(at(f_.healthNow), State("UnitStateLife", unit)));
        if (!f_.healthMax.empty()) out.push_back(Set(at(f_.healthMax), State("UnitStateMaxLife", unit)));
        if (!f_.manaNow.empty()) out.push_back(Set(at(f_.manaNow), State("UnitStateMana", unit)));
        if (!f_.manaMax.empty()) out.push_back(Set(at(f_.manaMax), State("UnitStateMaxMana", unit)));
        if (Portraits()) {
            out.push_back(Set(at(f_.portrait), Quote("")));
            Append(out, ForLoopVar(V("TypeI"), "1", V("TypeCount"),
                If({Call("OperatorCompareUnitCode", {Call("GetUnitTypeId", {unit}), "OperatorEqualENE", At("TypeCode", V("TypeI"))})},
                    {Set(at(f_.portrait), At("TypeIcon", V("TypeI")))})));
        }
        if (!f_.name.empty()) {
            if (nameSource_ == UnitNameSource::Player) {
                out.push_back(Set(at(f_.name), Call("GetPlayerName", {Call("GetOwningPlayer", {unit})})));
            } else {
                Append(out, If({IsTrue(Call("IsUnitType", {unit, "UnitTypeHero"}))}, {Set(at(f_.name), Call("GetHeroProperName", {unit}))},
                    {Set(at(f_.name), Call("GetUnitName", {unit}))}));
            }
        }
        if (!f_.dead.empty()) out.push_back(Set(at(f_.dead), Call("IsUnitDeadBJ", {unit})));
        if (Buffs()) Append(out, BuffIcons(unit, at));
        return out;
    }

    [[nodiscard]] static std::string State(std::string_view state, std::string_view unit) {
        return Call("GetUnitStateSwap", {std::string(state), std::string(unit)});
    }

private:
    [[nodiscard]] std::string V(std::string_view name) const { return "udg_" + prefix_ + "_" + std::string(name); }
    [[nodiscard]] std::string At(std::string_view name, std::string_view index) const {
        return V(name) + "[" + std::string(index) + "]";
    }
    void Error(std::vector<Diagnostic>& errors, std::string_view kind, const std::string& message) const {
        const std::string code = code_ + "_" + std::string(kind) + "_INVALID";
        errors.push_back(Note(DiagnosticSeverity::Error, code.c_str(), message));
    }

    void CheckBuffs(std::vector<Diagnostic>& errors) const {
        for (std::size_t i = 0; i < f_.buffIcons.size(); ++i) {
            if (const auto problem = CheckValueBinding(d_, f_.buffIcons[i], UiValueType::String, size_, "字符串")) Error(errors, "FRAME", *problem);
            for (std::size_t j = 0; j < i; ++j) {
                if (f_.buffIcons[j] == f_.buffIcons[i]) Error(errors, "FRAME", "图标数值 " + f_.buffIcons[i] + " 绑定了两次");
            }
        }
        if (!Buffs()) {
            if (!buffs_.empty()) Error(errors, "OPTION", "登记了增减益，但没有绑定图标数值");
            return;
        }
        if (buffs_.size() > 64) Error(errors, "BUFF", "最多登记 64 个增减益");
        for (std::size_t i = 0; i < buffs_.size(); ++i) {
            const UnitBuff& buff = buffs_[i];
            const std::string at = "第 " + std::to_string(i + 1) + " 个增减益（" + buff.buff + "）：";
            if (!ValidRawcode(buff.buff)) Error(errors, "BUFF", at + "增减益码应为 4 个字符");
            if (!UsableLabel(buff.icon)) Error(errors, "BUFF", at + "需要图标路径");
            for (std::size_t j = 0; j < i; ++j) {
                if (buffs_[j].buff == buff.buff) Error(errors, "BUFF", at + "已在第 " + std::to_string(j + 1) + " 个登记");
            }
        }
    }

    void CheckPortraits(std::vector<Diagnostic>& errors) const {
        if (!Portraits()) {
            if (!portraits_.empty()) Error(errors, "OPTION", "登记了头像，但没有绑定头像数值");
            return;
        }
        if (portraits_.size() > 256) Error(errors, "PORTRAIT", "最多登记 256 个头像");
        for (std::size_t i = 0; i < portraits_.size(); ++i) {
            const UnitPortrait& portrait = portraits_[i];
            const std::string at = "第 " + std::to_string(i + 1) + " 个头像（" + portrait.unit + "）：";
            if (!ValidRawcode(portrait.unit)) Error(errors, "PORTRAIT", at + "单位类型应为 4 个字符");
            if (!UsableLabel(portrait.icon)) Error(errors, "PORTRAIT", at + "需要图标路径");
            for (std::size_t j = 0; j < i; ++j) {
                if (portraits_[j].unit == portrait.unit) Error(errors, "PORTRAIT", at + "已在第 " + std::to_string(j + 1) + " 个登记");
            }
        }
    }

    // 比例 = 当前 ÷ 最大；最大为 0 时为 0。
    [[nodiscard]] static Lines Ratio(const std::string& target, std::string_view current, std::string_view maximum, const std::string& unit) {
        return If({CompareReal(State(maximum, unit), "OperatorGreater", "0")},
            {Set(target, Arith(State(current, unit), "OperatorDivide", State(maximum, unit)))}, {Set(target, "0")});
    }

    // 按登记顺序收集单位身上的增减益图标，填进各图标数值，多出的为空。
    template <typename CellRef>
    [[nodiscard]] Lines BuffIcons(const std::string& unit, const CellRef& at) const {
        const std::string slots = std::to_string(f_.buffIcons.size());
        Lines out;
        for (std::size_t k = 1; k <= f_.buffIcons.size(); ++k) out.push_back(Set(At("BuffHit", std::to_string(k)), Quote("")));
        out.push_back(Set(V("BuffN"), "0"));
        Append(out, ForLoopVar(V("BuffI"), "1", V("BuffCount"),
            If({CompareInt(V("BuffN"), "OperatorLess", slots), IsTrue(Call("UnitHasBuffBJ", {unit, At("BuffCode", V("BuffI"))}))},
                {Set(V("BuffN"), Int(V("BuffN"), "OperatorAdd", "1")), Set(At("BuffHit", V("BuffN")), At("BuffIcon", V("BuffI")))})));
        for (std::size_t k = 0; k < f_.buffIcons.size(); ++k) out.push_back(Set(at(f_.buffIcons[k]), At("BuffHit", std::to_string(k + 1))));
        return out;
    }

    std::string prefix_;
    std::string code_;
    const UiDescription& d_;
    UnitDisplayFields f_;
    std::uint32_t size_ = 0;
    UnitNameSource nameSource_ = UnitNameSource::Player;
    const std::vector<UnitBuff>& buffs_;
    const std::vector<UnitPortrait>& portraits_;
};

} // namespace War3Map::GameUi::detail
