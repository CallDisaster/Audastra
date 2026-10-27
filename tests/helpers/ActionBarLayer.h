#pragma once

#include "AudastraUiCatalog.h"

// 技能栏示例测试的合成签名层：选中判断、按热键、技能等级（原版 TriggerData），以及 YDWE 的技能属性（JAPI，层名以 ydwe 开头，
// 按游戏脚本检查时写成桩）。放在 SpellLayer、PartyLayer、TalentLayer、TargetLayer 之后（单位、技能码、单位类型、选择事件、
// 法力等取自前几层）。
namespace AudastraUiTest {

inline Catalog::Layer ActionBarLayer() {
    Catalog::Layer layer;
    layer.name = "actionbar";
    layer.action = "[ForceUIKeyBJ]\n[[.args]]\ntype=player\n[[.args]]\ntype=string\n";
    layer.call =
        "[IsUnitSelected]\nreturns=boolean\n[[.args]]\ntype=unit\n[[.args]]\ntype=player\n"
        "[GetUnitAbilityLevelSwapped]\nreturns=integer\n[[.args]]\ntype=abilcode\n[[.args]]\ntype=unit\n";
    return layer;
}

inline Catalog::Layer YdweAbilityStateLayer() {
    Catalog::Layer layer;
    layer.name = "ydwe-japi";
    layer.define =
        "[TriggerTypes]\n"
        "AbilityState=0,0,0,AbilityState,integer\nAbilityDataInteger=0,0,0,AbilityDataInteger,integer\n"
        "AbilityDataReal=0,0,0,AbilityDataReal,integer\n"
        "[TriggerParams]\n"
        "ABILITY_STATE_COOLDOWN=0,AbilityState,1,Cooldown\nABILITY_DATA_COST=0,AbilityDataInteger,104,Cost\n"
        "ABILITY_DATA_COOL=0,AbilityDataReal,105,Cool\n";
    layer.call =
        "[YDWEGetUnitAbilityState]\nreturns=real\n[[.args]]\ntype=unit\n[[.args]]\ntype=abilcode\n[[.args]]\ntype=AbilityState\n"
        "[YDWEGetUnitAbilityDataInteger]\nreturns=integer\n[[.args]]\ntype=unit\n[[.args]]\ntype=abilcode\n[[.args]]\ntype=integer\n"
        "[[.args]]\ntype=AbilityDataInteger\n"
        "[YDWEGetUnitAbilityDataReal]\nreturns=real\n[[.args]]\ntype=unit\n[[.args]]\ntype=abilcode\n[[.args]]\ntype=integer\n"
        "[[.args]]\ntype=AbilityDataReal\n";
    return layer;
}

} // namespace AudastraUiTest
