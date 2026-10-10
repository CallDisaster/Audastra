#pragma once

#include "War3MapCore/ObjectData/ObjectTypes.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace War3Map { class MapDocument; }

namespace War3Map::ObjectData {

class ObjectFieldSchema;

// 输入状态由调用方从原始成员解析结果提供；空记录只在 Ready 时代表已知的零对象。
enum class ReferenceInputState : std::uint8_t {
    Missing,
    Ready,
    Partial,
    Invalid,
    Ambiguous,
    Unreadable,
};

struct ObjectRecordSource {
    ObjectType type = ObjectType::Unit;
    std::string memberName;
    ReferenceInputState state = ReferenceInputState::Missing;
    std::span<const ObjectRecord> records;
};

enum class IndexedReferenceOrigin : std::uint8_t {
    ObjectParent,
    ObjectField,
    PlacedUnitOrItem,
    PlacedDoodadOrDestructable,
    SpecialDoodadOrDestructable,
    PlacedAbility,
    PlacedInventoryItem,
    PlacedDropItem,
    RandomUnitChoice,
};

// targetTypes 是按 ObjectType 排序的候选类型。只有一项且 exactType=true 才是
// 已知目标类型；其他命中均须在 UI 中标示为「可能引用」。
struct IndexedObjectReference {
    std::string rawcode;
    std::vector<ObjectType> targetTypes;
    bool exactType = false;
    IndexedReferenceOrigin origin = IndexedReferenceOrigin::ObjectField;
    std::string memberName;
    std::string location;
    // 结构化来源，供界面分组与跳转：物编父对象 / 字段时为所属对象与字段 ID，
    // 放置实例时为实例序号（特殊装饰物同样使用其序号）。
    std::optional<ObjectKey> ownerObject;
    std::string fieldId;
    std::optional<std::size_t> placementIndex;
};

struct ReferenceInputCoverage {
    std::string memberName;
    ReferenceInputState state = ReferenceInputState::Missing;
    std::string detail;
};

struct ObjectReferenceMatches {
    std::vector<IndexedObjectReference> definite;
    std::vector<IndexedObjectReference> possible;
};

// 只读的静态索引；不扫描 WTG/JASS/动态对象创建，不能生成删除审计 token。
// 有效查询仅表明已识别输入中的命中，不以空查询结果证明对象可删除。
class ObjectReferenceIndex {
public:
    [[nodiscard]] static ObjectReferenceIndex Build(
        std::span<const ObjectRecordSource> objectSources,
        const MapDocument& map,
        const ObjectFieldSchema* schema = nullptr);

    [[nodiscard]] std::span<const IndexedObjectReference> References() const noexcept { return references_; }
    [[nodiscard]] std::span<const ReferenceInputCoverage> Coverage() const noexcept { return coverage_; }
    [[nodiscard]] ObjectReferenceMatches Find(const ObjectKey& target) const;

private:
    std::vector<IndexedObjectReference> references_;
    std::vector<ReferenceInputCoverage> coverage_;
};

} // namespace War3Map::ObjectData
