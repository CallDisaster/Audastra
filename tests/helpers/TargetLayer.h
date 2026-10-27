#pragma once

#include "AudastraUiCatalog.h"

// 目标框架示例测试的合成签名层：取消选择的选项与单位等级。名字、参数类型与预设照原版 TriggerData；
// 放在 SpellLayer、PartyLayer、TalentLayer 之后（选择事件、英雄等级等取自前几层）。
namespace AudastraUiTest {

inline Catalog::Layer TargetLayer() {
    Catalog::Layer layer;
    layer.name = "target";
    layer.define = "[TriggerParams]\nSelDeselOptionDesel=0,seldeseloption,false,Deselect\n";
    layer.call = "[GetUnitLevel]\nreturns=integer\n[[.args]]\ntype=unit\n";
    return layer;
}

} // namespace AudastraUiTest
