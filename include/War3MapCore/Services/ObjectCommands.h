#pragma once
#include "War3MapCore/Services/NameResolution.h"

#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Services/ObjectDeletionAssessment.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace War3Map::Services {

// 领域命令借用当前已确认的只读来源。不存在的来源不能用猜测的空目录补齐。
struct ObjectCommandContext {
    ObjectData::ObjectType type = ObjectData::ObjectType::Unit;
    const ObjectData::LniObjectDocument* document = nullptr;
    const ObjectData::StandardObjectCatalog* standard = nullptr;
    const ObjectData::ObjectFieldSchema* schema = nullptr;
    std::array<const ObjectData::LniObjectDocument*, 7> documents{};
    const MapDocument* nameMap = nullptr; // 当前 WTS overlay 的借用视图，仅用于名称展示。
};

enum class ObjectListScope { Standard, Custom, All };

struct ObjectListRow {
    ObjectData::ObjectKey key;
    StableId stableId = 0;
    std::string name;
    std::string source; // standard / map_override / custom
    std::optional<std::string> parent;
    NameResolution nameResolution;
};

struct ObjectFieldRow {
    ObjectData::ModificationKey key;
    std::string sourceKey;
    const ObjectData::ObjectFieldDefinition* definition = nullptr;
    std::optional<ObjectData::Value> value;
    std::optional<ObjectData::Value> defaultValue;
    bool overridden = false;
    bool editable = false;
    std::string valueSource;
    std::string reason;
};

struct ObjectFieldChange {
    std::string field;
    std::optional<std::uint32_t> level;
    std::optional<std::uint32_t> variation;
    ObjectData::Value value;
};

// 只有全部字段校验、LNI 序列化与复解析完成，才返回拥有前后状态的计划。
// 宿主在核对 expected_revision 后，一次进入原有撤销栈或 EditorSession。
struct ObjectCommandPlan {
    ObjectData::ObjectKey subject;
    ObjectData::LniObjectDocument before;
    ObjectData::LniObjectDocument after;
    ObjectData::ObjectSemanticDiff changes;
    std::string label;
};

[[nodiscard]] Result<std::vector<ObjectListRow>> ListObjects(
    const ObjectCommandContext& context, ObjectListScope scope);
[[nodiscard]] Result<std::vector<ObjectFieldRow>> ReadObjectFields(
    const ObjectCommandContext& context, const std::string& id);
[[nodiscard]] Result<ObjectData::ObjectFieldMatch> ResolveObjectField(
    const ObjectCommandContext& context, const std::string& id,
    const std::string& field, std::optional<std::uint32_t> level = {},
    std::optional<std::uint32_t> variation = {});
[[nodiscard]] Result<ObjectCommandPlan> PlanObjectFieldChanges(
    const ObjectCommandContext& context, const std::string& id,
    std::span<const ObjectFieldChange> changes);
[[nodiscard]] Result<ObjectCommandPlan> PlanObjectCreation(
    const ObjectCommandContext& context, const std::string& baseId,
    const std::optional<std::string>& newId, const std::optional<std::string>& name);
// 当前审计未能证明动态/脚本全覆盖时明确拒绝，不从零静态引用构造删除令牌。
[[nodiscard]] Result<ObjectCommandPlan> PlanObjectDeletion(
    const ObjectCommandContext& context, const std::string& id,
    const ObjectDeletionAssessment& assessment);
} // namespace War3Map::Services
