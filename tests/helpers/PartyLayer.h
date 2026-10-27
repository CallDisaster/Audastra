#pragma once

#include "AudastraUiCatalog.h"

// 团队框架测试的合成签名层：单位属性、单位分类与单位类型、玩家控制者与状态、可用地图区域与单位组、选择单位等函数。
// 名字、参数类型与预设照原版 TriggerData；放在读条测试的 SpellLayer 之后（单位、玩家、点、计时器类型取自那一层）。
namespace AudastraUiTest {

inline Catalog::Layer PartyLayer() {
    Catalog::Layer layer;
    layer.name = "party";
    layer.define =
        "[TriggerTypes]\n"
        "unitstate=0,0,0,UnitState\nunittype=0,0,0,UnitType\nmapcontrol=0,0,1,MapControl\nplayerslotstate=0,0,1,PlayerSlotState\n"
        "rect=0,1,1,Rect\ngroup=0,1,0,Group\nbuffcode=1,1,1,Buff,integer\nunitcode=0,1,1,UnitType,integer\n"
        "[TriggerParams]\n"
        "UnitStateLife=0,unitstate,UNIT_STATE_LIFE,Life\nUnitStateMaxLife=0,unitstate,UNIT_STATE_MAX_LIFE,MaxLife\n"
        "UnitStateMana=0,unitstate,UNIT_STATE_MANA,Mana\nUnitStateMaxMana=0,unitstate,UNIT_STATE_MAX_MANA,MaxMana\n"
        "UnitTypeHero=0,unittype,UNIT_TYPE_HERO,Hero\nMapControlUser=0,mapcontrol,MAP_CONTROL_USER,User\n"
        "PlayerSlotStatePlaying=0,playerslotstate,PLAYER_SLOT_STATE_PLAYING,Playing\nUnitNull=0,unit,null,NoUnit\n"
        "OperatorLessEq=0,ComparisonOperator,<=,LessEq\n";
    layer.event = "[TriggerRegisterEnterRectSimple]\n[[.args]]\ntype=rect\n";
    layer.condition =
        "[OperatorComparePlayerControl]\nreturns=boolean\n[[.args]]\ntype=mapcontrol\n[[.args]]\ntype=EqualNotEqualOperator\n[[.args]]\ntype=mapcontrol\n"
        "[OperatorComparePlayerSlotStatus]\nreturns=boolean\n[[.args]]\ntype=playerslotstate\n[[.args]]\ntype=EqualNotEqualOperator\n"
        "[[.args]]\ntype=playerslotstate\n"
        "[OperatorCompareUnitCode]\nreturns=boolean\n[[.args]]\ntype=unitcode\n[[.args]]\ntype=EqualNotEqualOperator\n[[.args]]\ntype=unitcode\n";
    layer.action =
        "[ForGroupMultiple]\n[[.args]]\ntype=group\n"
        "[SelectUnitForPlayerSingle]\n[[.args]]\ntype=unit\n[[.args]]\ntype=player\n";
    layer.call =
        "[UnitHasBuffBJ]\nreturns=boolean\n[[.args]]\ntype=unit\n[[.args]]\ntype=buffcode\n"
        "[GetUnitStateSwap]\nreturns=real\n[[.args]]\ntype=unitstate\n[[.args]]\ntype=unit\n"
        "[IsUnitType]\nreturns=boolean\n[[.args]]\ntype=unit\n[[.args]]\ntype=unittype\n"
        "[IsUnitIllusionBJ]\nreturns=boolean\n[[.args]]\ntype=unit\n"
        "[GetPlayerController]\nreturns=mapcontrol\n[[.args]]\ntype=player\n"
        "[GetPlayerSlotState]\nreturns=playerslotstate\n[[.args]]\ntype=player\n"
        "[GetPlayableMapRect]\nreturns=rect\n[GetUnitsInRectAll]\nreturns=group\n[[.args]]\ntype=rect\n[GetEnumUnit]\nreturns=unit\n"
        "[GetUnitLoc]\nreturns=location\n[[.args]]\ntype=unit\n[GetUnitTypeId]\nreturns=unitcode\n[[.args]]\ntype=unit\n"
        "[GetPlayerName]\nreturns=string\n[[.args]]\ntype=player\n"
        "[GetHeroProperName]\nreturns=string\n[[.args]]\ntype=unit\n[GetUnitName]\nreturns=string\n[[.args]]\ntype=unit\n";
    return layer;
}

} // namespace AudastraUiTest
