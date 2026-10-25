#pragma once

#include "AudastraUiCatalog.h"

// 读条系统测试共用的合成签名层：施法事件、单位与计时器函数、YDWE 运行触发器参数块（参数类型按 TriggerData 写法，
// 名字与预设取自真实地图里用过的写法）。
namespace AudastraUiTest {

inline Catalog::Layer SpellLayer() {
    Catalog::Layer layer;
    layer.name = "spell";
    layer.define =
        "[TriggerTypes]\n"
        "unit=0,1,1,Unit\nplayer=0,1,1,Player\ntimer=0,1,1,Timer\nlocation=0,1,1,Location\n"
        "abilcode=0,1,1,Ability,integer\nintegervar=1,0,0,IntegerVariable,integer\n"
        "playerunitevent=0,0,0,PlayerUnitEvent\nperiodicoption=0,0,0,Periodic,boolean\ntypename=0,0,0,TypeName\n"
        "executemode=0,0,0,ExecuteMode,boolean\n"
        "[TriggerParams]\n"
        "PlayerUnitEventSpellChannel=0,playerunitevent,EVENT_PLAYER_UNIT_SPELL_CHANNEL,Channel\n"
        "PlayerUnitEventSpellEffect=0,playerunitevent,EVENT_PLAYER_UNIT_SPELL_EFFECT,Effect\n"
        "PlayerUnitEventSpellFinish=0,playerunitevent,EVENT_PLAYER_UNIT_SPELL_FINISH,Finish\n"
        "PlayerUnitEventSpellEndCast=0,playerunitevent,EVENT_PLAYER_UNIT_SPELL_ENDCAST,EndCast\n"
        "PeriodicOptionOneTime=0,periodicoption,false,OneTime\n"
        "typename01_integer=0,typename,integer,Integer\ntypename02_real=0,typename,real,Real\n"
        "typename07_unit=0,typename,unit,Unit\n"
        "tirggerexecute_notcondition=0,executemode,false,NoCondition\n"
        "OperatorSubtract=0,ArithmeticOperator,-,Subtract\nOperatorDivide=0,ArithmeticOperator,/,Divide\n"
        "OperatorLess=0,ComparisonOperator,<,Less\n"
        "OperatorGreaterEq=0,ComparisonOperator,>=,GreaterEq\n"
        "[TriggerTypeDefaults]\ntimer=CreateTimer()\n";
    layer.event =
        "[TriggerRegisterAnyUnitEventBJ]\n[[.args]]\ntype=playerunitevent\n";
    layer.condition =
        "[OperatorCompareAbilityId]\nreturns=boolean\n[[.args]]\ntype=abilcode\n[[.args]]\ntype=EqualNotEqualOperator\n[[.args]]\ntype=abilcode\n"
        "[OperatorCompareUnit]\nreturns=boolean\n[[.args]]\ntype=unit\n[[.args]]\ntype=EqualNotEqualOperator\n[[.args]]\ntype=unit\n"
        "[OperatorComparePlayer]\nreturns=boolean\n[[.args]]\ntype=player\n[[.args]]\ntype=EqualNotEqualOperator\n[[.args]]\ntype=player\n";
    layer.action =
        "[StartTimerBJ]\n[[.args]]\ntype=timer\n[[.args]]\ntype=periodicoption\n[[.args]]\ntype=real\n"
        "[ConditionalTriggerExecute]\n[[.args]]\ntype=trigger\n"
        "[RemoveLocation]\n[[.args]]\ntype=location\n"
        "[ForLoopVarMultiple]\n[[.args]]\ntype=integervar\n[[.args]]\ntype=integer\n[[.args]]\ntype=integer\n"
        "[YDWEExecuteTriggerMultiple]\n[[.args]]\ntype=trigger\n[[.args]]\ntype=executemode\n"
        "[YDWESetAnyTypeLocalVariable]\n[[.args]]\ntype=typename\n[[.args]]\ntype=scriptcode\n[[.args]]\ntype=Null\n";
    layer.call =
        "[GetSpellAbilityId]\nreturns=abilcode\n[GetTriggerUnit]\nreturns=unit\n[GetSpellTargetUnit]\nreturns=unit\n"
        "[GetSpellTargetLoc]\nreturns=location\n"
        "[GetLocationX]\nreturns=real\n[[.args]]\ntype=location\n[GetLocationY]\nreturns=real\n[[.args]]\ntype=location\n"
        "[GetUnitAbilityLevel]\nreturns=integer\n[[.args]]\ntype=unit\n[[.args]]\ntype=abilcode\n"
        "[TimerGetElapsed]\nreturns=real\n[[.args]]\ntype=timer\n"
        "[R2I]\nreturns=integer\n[[.args]]\ntype=real\n"
        "[IsUnitDeadBJ]\nreturns=boolean\n[[.args]]\ntype=unit\n"
        "[GetOwningPlayer]\nreturns=player\n[[.args]]\ntype=unit\n[GetLocalPlayer]\nreturns=player\n"
        "[GetConvertedPlayerId]\nreturns=integer\n[[.args]]\ntype=player\n"
        "[GetAbilityName]\nreturns=string\n[[.args]]\ntype=abilcode\n"
        "[YDWEGetAnyTypeLocalVariable]\nreturns=AnyReturnType\n[[.args]]\ntype=scriptcode\n";
    return layer;
}

} // namespace AudastraUiTest
