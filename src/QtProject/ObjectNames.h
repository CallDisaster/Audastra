#pragma once

#include "War3MapCore/ObjectData/ObjectTypes.h"
#include "War3MapCore/Services/NameResolution.h"
#include "War3MapCore/Trigger/TriggerPresentation.h"

#include <QString>

#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::ObjectData {
class LniObjectDocument;
class NativeObjectDocument;
class StandardObjectCatalog;
struct StandardObjectEntry;
}

namespace War3Map { class MapDocument; }
namespace War3Map::QtProject { class EditorProject; }

struct ObjectPresentation {
    QString name;
    QString iconPath;
    QString issue;
    QString nameReference;
};

// 当前工程 overlay 的共用 N1 解析适配；只借用物编来源，缓存地图数据快照随会话/修订失效。
// 查询不写回 rawValue，也不取得编辑资格；无法解析时仍返回原值及明确原因。
[[nodiscard]] War3Map::Services::NameResolution ResolveProjectName(
    const War3Map::QtProject::EditorProject& project, std::string_view rawValue,
    std::string_view uiType = {});
// 字符串表引用（TRIGSTR_n）在当前工程 overlay 中的原始正文，不去颜色码或换行，供编辑框使用。
// 不是引用、字符串表未加载或条目缺失/重复时返回空，调用方照旧显示原值。
[[nodiscard]] std::optional<std::string> ResolveProjectStringText(
    const War3Map::QtProject::EditorProject& project, std::string_view rawValue);
[[nodiscard]] ObjectPresentation ResolveProjectObjectPresentation(
    const War3Map::QtProject::EditorProject& project, const War3Map::ObjectData::ObjectKey& key);
[[nodiscard]] War3Map::Trigger::ReferencePresentation ResolveProjectReferencePresentation(
    const War3Map::QtProject::EditorProject& project, std::string_view rawValue, std::string_view uiType = {});

// 地图覆盖 → 自定义父链 → 原版目录。供物编、实例与选择器共用显示身份。
[[nodiscard]] ObjectPresentation ResolveObjectPresentation(
    const War3Map::ObjectData::ObjectKey& key,
    const War3Map::ObjectData::LniObjectDocument* document,
    const War3Map::ObjectData::StandardObjectCatalog* catalog,
    const War3Map::MapDocument* map = nullptr);

// 原生IR只读入口，复用同一名称/图标/父链与WTS规则；不替换当前LNI覆盖。
[[nodiscard]] ObjectPresentation ResolveNativeObjectPresentation(
    const War3Map::ObjectData::ObjectKey& key,
    const War3Map::ObjectData::NativeObjectDocument* document,
    const War3Map::ObjectData::StandardObjectCatalog* catalog,
    const War3Map::MapDocument* map = nullptr);

// 摆放（单位文件含单位/物品，装饰物文件含装饰物/可破坏物）的显示身份：只在两类目录中恰好
// 一类认得该 rawcode 时确定类型，未知或歧义不猜测；起始位置单独命名。实例列表与世界视图共用。
[[nodiscard]] std::optional<War3Map::ObjectData::ObjectType> PlacementType(const War3Map::QtProject::EditorProject& project,
    const std::string& rawcode, War3Map::ObjectData::ObjectType first, War3Map::ObjectData::ObjectType second);
[[nodiscard]] ObjectPresentation PlacementPresentation(const War3Map::QtProject::EditorProject& project,
    const War3Map::MapDocument& snapshot, const std::string& rawcode,
    War3Map::ObjectData::ObjectType first, War3Map::ObjectData::ObjectType second);

// 只读父链快照，records 从选中对象到最远地图父对象排列。
// 指针仅在文档/目录未变更期间有效；调用方不得将展示字段回退视为继承。
struct ObjectInheritanceChain {
    std::vector<const War3Map::ObjectData::ObjectRecord*> records;
    const War3Map::ObjectData::StandardObjectEntry* standardBase = nullptr;
    QString reason;
    bool catalogUnavailable = false;
    [[nodiscard]] bool IsComplete() const { return reason.isEmpty(); }
};
[[nodiscard]] ObjectInheritanceChain ResolveObjectInheritance(
    const War3Map::ObjectData::LniObjectDocument& document,
    const War3Map::ObjectData::ObjectKey& object,
    const War3Map::ObjectData::StandardObjectCatalog* catalog);
[[nodiscard]] ObjectInheritanceChain ResolveNativeObjectInheritance(
    const War3Map::ObjectData::NativeObjectDocument& document,
    const War3Map::ObjectData::ObjectKey& object,
    const War3Map::ObjectData::StandardObjectCatalog* catalog);
// 父链上字段的有效值：从选中对象向父对象找第一个覆盖（字段 ID 不区分大小写，可给 LNI 名与原生 ID 多个别名），
// 都没有覆盖时取原版目录。链不完整时只返回链上已有的覆盖，不回退原版。
[[nodiscard]] std::optional<std::string> ObjectFieldValue(const ObjectInheritanceChain& chain,
    std::initializer_list<std::string_view> fieldIds);
