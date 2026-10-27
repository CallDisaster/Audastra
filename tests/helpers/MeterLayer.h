#pragma once

#include "AudastraUiCatalog.h"

// 伤害统计示例测试的合成签名层：伤害事件的取值与敌对判断（原版 TriggerData），以及 YDWE 的任意单位伤害事件
// （层名以 ydwe 开头，按游戏脚本检查时写成桩）。放在 SpellLayer、PartyLayer、TalentLayer 之后（单位、玩家、玩家名、
// 转换玩家编号等取自前几层）。
namespace AudastraUiTest {

inline Catalog::Layer MeterLayer() {
    Catalog::Layer layer;
    layer.name = "meter";
    layer.call =
        "[GetEventDamage]\nreturns=real\n[GetEventDamageSource]\nreturns=unit\n"
        "[IsUnitEnemy]\nreturns=boolean\n[[.args]]\ntype=unit\n[[.args]]\ntype=player\n";
    return layer;
}

inline Catalog::Layer YdweDamageLayer() {
    Catalog::Layer layer;
    layer.name = "ydwe-damage";
    layer.event = "[YDWESyStemAnyUnitDamagedRegistTrigger]\n";
    return layer;
}

} // namespace AudastraUiTest
