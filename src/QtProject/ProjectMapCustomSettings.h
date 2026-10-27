#pragma once

#include "War3MapCore/Services/MapCustomSettingsEditing.h"
#include <QString>
#include <string>

// 地图科技/随机表编辑的工程接线：地图设置页与活动 Agent 工具共用同一来源校验和同一撤销提交。
namespace War3Map::QtProject {
class EditorProject;
// 物编来源取自真实层序与已打开的物编文档副本；工程修订或游戏目录变化后 verify 失败。
// 原生物编导入/删除尚未进入物编文档时拒绝，避免用旧文档授权新对象。
[[nodiscard]] Result<Services::MapCustomSettingsValidation> CaptureProjectMapCustomSettingsValidation(EditorProject& project);
// 经共用地图数据会话提交已准备的批次，一个批次一个撤销记录；无变化时不增加记录。
[[nodiscard]] Result<MapData::MapDataEditSession::EditReceipt> ApplyProjectMapCustomSettings(EditorProject& project,
    const Services::PreparedMapCustomSettingsEdit& plan, const Services::MapCustomSettingsValidation& validation,
    const QString& undoLabel, const std::string& description, const MapData::MapDataPublishValidator& validator = {});
} // namespace War3Map::QtProject
