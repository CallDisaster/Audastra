#pragma once

#include "Widgets/ObjectCatalogWidgets.h"
#include "War3MapCore/ObjectData/ObjectTypes.h"

#include <QVector>

namespace War3Map::QtProject { class EditorProject; }

namespace War3Map::QtApp {

enum class ObjectViewSource { All, Map, Standard };
// 与物编目录共用的有效分类；地图父链缺失时不猜英雄或中立阵营。
struct ObjectViewClassification {
    QStringList folders;
    std::optional<bool> hero;
    QString issue;
};
[[nodiscard]] ObjectViewClassification ResolveObjectViewClassification(
    const QtProject::EditorProject& project, const ObjectData::ObjectKey& object);
struct ObjectViewRow {
    ObjectViewEntry entry;
    ObjectData::ObjectKey object;
    bool mapObject = false;
    bool customObject = false;
    ObjectViewClassification classification;
};
struct ObjectViewQuery {
    QString kind, search, group;
    ObjectViewSource source = ObjectViewSource::All;
    std::size_t offset = 0, limit = 64, maximumExamined = 4096;
    QString catalogSet; // 单位目录的“对战 / 战役”，空值不筛选。
    QString catalogCategory;
    std::size_t maximumIndexBytes = 64 * 1024 * 1024; // 借用索引的内存估算，不限制地图实例数量。
};
struct ObjectViewPage {
    QVector<ObjectViewRow> rows;
    std::size_t nextOffset = 0, examined = 0;
    bool hasMore = false, partial = false;
    QString issue;
    bool preparing = false; // 冷索引构建中；nextOffset 可不变，调用者应按同一来源继续查询。
};

// 当前IR借用查询，地图先于标准；冷索引与分页共用工作预算，按来源修订缓存。
// preparing 时 nextOffset 可不变；调用方须继续查询，不能把暂未出行当作空目录。
[[nodiscard]] ObjectViewPage QueryObjectViewEntries(const QtProject::EditorProject& project,
    const ObjectViewQuery& query);

// 物体目录条目：原版目录 + 地图覆盖/自定义对象（同 rawcode 的地图条目覆盖原版条目）。
// 物编引用取值、世界视图摆放面板共用；standardOnly 非空时另外输出仅原版的条目。
[[nodiscard]] QVector<ObjectViewEntry> BuildObjectViewEntries(const War3Map::QtProject::EditorProject& project,
    QVector<ObjectViewEntry>* standardOnly = nullptr);

} // namespace War3Map::QtApp
