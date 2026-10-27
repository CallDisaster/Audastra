#pragma once

#include "AudastraUiCatalog.h"

// 天赋树示例测试的合成签名层：玩家选择单位事件、建单位与单位属性、英雄等级、选择增减、学技能等函数。
// 名字、参数类型与预设照原版 TriggerData；放在 SpellLayer、PartyLayer 之后（单位、单位类型、玩家、点、区域、技能类型取自那两层）。
namespace AudastraUiTest {

inline Catalog::Layer TalentLayer() {
    Catalog::Layer layer;
    layer.name = "talent";
    std::string players;
    for (int i = 0; i < 12; ++i) {
        const std::string index = (i < 10 ? "0" : "") + std::to_string(i);
        players += "Player" + index + "=0,player,Player(" + std::to_string(i) + "),Player" + index + "\n";
    }
    layer.define =
        "[TriggerTypes]\n"
        "seldeseloption=0,0,0,SelDesel,boolean\nenabledisableoption=0,0,0,EnableDisable,boolean\n"
        "invulnerableoption=0,0,0,Invulnerable,boolean\npathingoption=0,0,0,Pathing,boolean\npauseunpauseoption=0,0,0,PauseUnpause,boolean\n"
        "[TriggerParams]\n" + players +
        "SelDeselOptionSel=0,seldeseloption,true,Select\nEnableDisableDisable=0,enabledisableoption,false,Disable\n"
        "InvulnerabilityInvulnerable=0,invulnerableoption,true,Invulnerable\nPathingOff=0,pathingoption,false,Off\n"
        "PauseUnpauseOptionPause=0,pauseunpauseoption,true,Pause\n"
        "PlayerUnitEventHero_Level=0,playerunitevent,EVENT_PLAYER_HERO_LEVEL,HeroLevel\n";
    layer.event = "[TriggerRegisterPlayerSelectionEventBJ]\n[[.args]]\ntype=player\n[[.args]]\ntype=seldeseloption\n";
    layer.action =
        "[CreateNUnitsAtLoc]\n[[.args]]\ntype=integer\n[[.args]]\ntype=unitcode\n[[.args]]\ntype=player\n[[.args]]\ntype=location\n[[.args]]\ntype=real\n"
        "[SetUnitUseFoodBJ]\n[[.args]]\ntype=enabledisableoption\n[[.args]]\ntype=unit\n"
        "[SetUnitInvulnerable]\n[[.args]]\ntype=unit\n[[.args]]\ntype=invulnerableoption\n"
        "[SetUnitPathing]\n[[.args]]\ntype=unit\n[[.args]]\ntype=pathingoption\n"
        "[SetUnitPositionLoc]\n[[.args]]\ntype=unit\n[[.args]]\ntype=location\n"
        "[SetUnitVertexColorBJ]\n[[.args]]\ntype=unit\n[[.args]]\ntype=real\n[[.args]]\ntype=real\n[[.args]]\ntype=real\n[[.args]]\ntype=real\n"
        "[PauseUnitBJ]\n[[.args]]\ntype=pauseunpauseoption\n[[.args]]\ntype=unit\n"
        "[SelectUnitAddForPlayer]\n[[.args]]\ntype=unit\n[[.args]]\ntype=player\n"
        "[SelectUnitRemoveForPlayer]\n[[.args]]\ntype=unit\n[[.args]]\ntype=player\n"
        "[UnitAddAbilityBJ]\n[[.args]]\ntype=abilcode\n[[.args]]\ntype=unit\n";
    layer.call =
        "[GetEntireMapRect]\nreturns=rect\n[GetRectMinX]\nreturns=real\n[[.args]]\ntype=rect\n[GetRectMinY]\nreturns=real\n[[.args]]\ntype=rect\n"
        "[Location]\nreturns=location\n[[.args]]\ntype=real\n[[.args]]\ntype=real\n"
        "[GetLastCreatedUnit]\nreturns=unit\n[ConvertedPlayer]\nreturns=player\n[[.args]]\ntype=integer\n"
        "[GetHeroLevel]\nreturns=integer\n[[.args]]\ntype=unit\n"
        "[GetTriggerPlayer]\nreturns=player\n";
    return layer;
}

} // namespace AudastraUiTest
