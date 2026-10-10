#pragma once

#include "War3MapCore/ObjectData/ObjectTypes.h"

#include <QString>
#include <QStringList>

#include <functional>
#include <string>
#include <string_view>

namespace War3Map::ObjectData { struct StandardObjectEntry; }
struct ObjectInheritanceChain;

// 原版物体编辑器的目录分类（与 WE 一致）：
//   单位：种族 → 对战 / 战役 → 单位 / 建筑 / 英雄 / 特殊
//   技能：种族 → 单位 / 英雄；物品技能单独一个“物品”文件夹
//   魔法效果：种族 → 魔法效果 / 效果；科技：种族
//   物品：物品类别；装饰物 / 可破坏物：类别
// 标准对象读 SLK 列；自定义对象先看自身与地图父链的字段修改（原生字段 ID），再回落到原版基础对象。
namespace War3Map::Modules::ObjectDetail {

// “标准单位” / “自定义单位”等根文件夹名。
[[nodiscard]] QString RootLabel(ObjectData::ObjectType type, bool custom);

// 读取分类字段：slk 为原版 SLK / 配置列名（小写），native 为地图物编的原生字段 ID。
using ClassificationLookup = std::function<std::string(std::string_view slk, std::string_view native)>;
[[nodiscard]] QStringList Classify(ObjectData::ObjectType type, const std::string& rawcode, const ClassificationLookup& lookup);
[[nodiscard]] QStringList ClassifyStandard(const ObjectData::StandardObjectEntry& entry);
[[nodiscard]] QStringList ClassifyCustom(const ObjectInheritanceChain& chain, ObjectData::ObjectType type, const std::string& rawcode);

// 文件夹排序：按原版固定顺序（人族、兽族、…；对战、战役；单位、建筑、英雄、特殊）。未知的排在后面。
// depth 为文件夹在根下的层级（0 起）。
[[nodiscard]] int FolderRank(const QString& label, int depth);

} // namespace War3Map::Modules::ObjectDetail
