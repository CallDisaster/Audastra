#include "ObjectResourceQueryDetail.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"

#include <algorithm>
#include <unordered_map>

namespace War3Map::Services {
using namespace ObjectResourceDetail;
namespace {
using namespace ObjectData;
struct RecordSource { const ObjectRecord* record = nullptr; ProjectSearchObjectOrigin origin; bool ambiguous = false; };
using RecordIndex = std::map<std::pair<ObjectType, std::string>, RecordSource>;
struct FieldValue { const ObjectFieldDefinition* definition = nullptr; std::string value; };
using EffectiveFields = std::map<std::string, FieldValue>;

std::string FieldAddress(const ModificationKey& key) {
    auto text = key.fieldId;
    for (const auto value : {key.level, key.dataIndex, key.variation}) text += ':' + (value ? std::to_string(*value) : "-");
    return text;
}
ObjectFieldContext Context(const ObjectKey& key, const StandardObjectEntry* base) {
    ObjectFieldContext result;
    result.rawcode = base ? base->key.rawcode : key.rawcode;
    if (base) {
        result.baseCode = base->Field("code"); result.building = base->Field("isbldg") == "1";
        result.hero = !base->key.rawcode.empty() && base->key.rawcode.front() >= 'A' && base->key.rawcode.front() <= 'Z';
        result.item = key.type == ObjectType::Item;
    }
    return result;
}
bool HasKnownSource(const ObjectResourceContext& context, ObjectType type) {
    for (const auto& source : context.names.objects) if (source.type == type
        && source.state != ProjectSearchState::Ready && source.state != ProjectSearchState::Missing
        && source.state != ProjectSearchState::Partial) return false;
    return true;
}
void StoreSourceFields(const ObjectResourceContext& context, const ObjectKey& key, const ObjectFieldContext& fieldContext,
    std::string_view sourceKey, std::string_view rawValue, EffectiveFields& fields) {
    const auto first = context.schema->MatchSourceField(key.type, sourceKey, fieldContext);
    if (!first.definition || !DescribeObjectResourceField(*first.definition)) return;
    const bool profile = Text::LowerAscii(first.definition->sourceTable) == "profile";
    if (!profile || !first.sourceIndex) {
        fields[FieldAddress(first.modification)] = {first.definition,
            profile ? SlkText::DecodeProfileItem(rawValue) : std::string(rawValue)};
        return;
    }
    const auto tokens = SlkText::SplitProfileList(rawValue);
    for (std::size_t index = 0; index < tokens.size(); ++index) {
        const auto match = context.schema->MatchSourceField(key.type, sourceKey, fieldContext, static_cast<std::uint32_t>(index));
        if (!match.definition || !DescribeObjectResourceField(*match.definition) || match.sourceIndex != index) continue;
        fields[FieldAddress(match.modification)] = {match.definition, SlkText::DecodeProfileItem(tokens[index])};
    }
}
bool CaptureFields(const ObjectResourceContext& context, const RecordIndex& records, const ObjectKey& key,
    EffectiveFields& fields, Builder& builder) {
    std::vector<RecordSource> lineage;
    std::set<std::string> visited;
    std::string rawcode = key.rawcode;
    for (std::size_t depth = 0; depth < 256; ++depth) {
        if (!visited.insert(rawcode).second) {
            builder.Diagnose(Warning("OBJECT_RESOURCE_PARENT_CYCLE", "物体继承循环，未列出资源：" + key.rawcode));
            builder.Catalog().complete = false; return false;
        }
        const auto found = records.find({key.type, rawcode});
        if (found == records.end()) break;
        if (found->second.ambiguous) {
            builder.Diagnose(Warning("OBJECT_RESOURCE_OBJECT_AMBIGUOUS", "物体存在重复定义，未列出资源：" + key.rawcode));
            builder.Catalog().complete = false; return false;
        }
        lineage.push_back(found->second);
        const auto parent = found->second.record->parentRawcode.value_or(rawcode);
        if (parent.empty() || parent == rawcode) break;
        rawcode = parent;
        if (depth == 255) { builder.Exhausted(); return false; }
    }
    const auto* base = context.names.standardObjects ? context.names.standardObjects->Find(key.type, rawcode) : nullptr;
    const auto fieldContext = Context(key, base);
    if (base) {
        std::vector<std::string_view> keys;
        keys.reserve(base->fields.size());
        for (const auto& [name, value] : base->fields) keys.push_back(name);
        std::sort(keys.begin(), keys.end());
        for (const auto name : keys) {
            if (!builder.Tick()) return false;
            StoreSourceFields(context, key, fieldContext, name, base->Field(name), fields);
        }
    }
    for (auto it = lineage.rbegin(); it != lineage.rend(); ++it) {
        std::set<std::string> changed;
        for (const auto& modification : it->record->modifications) {
            if (!builder.Tick()) return false;
            const auto match = it->origin == ProjectSearchObjectOrigin::Native
                ? context.schema->MatchNativeField(key.type, modification.key, fieldContext)
                : context.schema->MatchSourceField(key.type, modification.sourceKey.empty()
                    ? modification.key.fieldId : modification.sourceKey, fieldContext);
            if (!match.definition || !DescribeObjectResourceField(*match.definition)) continue;
            const auto address = FieldAddress(match.modification);
            if (!changed.insert(address).second || modification.valueWasOpaque || std::holds_alternative<UnknownBytesValue>(modification.value)) {
                fields.erase(address);
                builder.Catalog().complete = false;
                builder.Diagnose(Warning("OBJECT_RESOURCE_FIELD_UNREADABLE", "资源字段有重复或未知值，未回退父值：" + key.rawcode + ':' + address));
                continue;
            }
            // 空串是有效覆盖；稍后不生成候选，不能补回原版值。
            if (it->origin == ProjectSearchObjectOrigin::Native)
                fields[address] = {match.definition, ValueToDisplayString(modification.value)};
            else StoreSourceFields(context, key, fieldContext, modification.sourceKey.empty()
                ? modification.key.fieldId : modification.sourceKey, ValueToDisplayString(modification.value), fields);
        }
    }
    return true;
}
}

Result<std::shared_ptr<const ObjectResourceCatalog>> CaptureObjectResourceCatalog(
    const ObjectResourceContext& context, const ObjectResourceCatalogLimits& limits) {
    using Return = Result<std::shared_ptr<const ObjectResourceCatalog>>;
    Builder builder(context.revision, ObjectResourceOrigin::Object, limits);
    if (!builder.Valid()) return builder.Finish();
    if (!context.schema) return Return::Failure("Object.Resource", "OBJECT_RESOURCE_SCHEMA_MISSING", "字段元数据未加载，无法判断资源字段类型。");
    RecordIndex records;
    std::set<std::pair<ObjectType, std::string>> objects;
    if (context.names.standardObjects) for (const auto& entry : context.names.standardObjects->Entries()) {
        if (!builder.Tick()) return builder.Finish();
        if (objects.size() >= limits.maxObjects) { builder.Exhausted(); break; }
        objects.emplace(entry.key.type, entry.key.rawcode);
    }
    else {
        builder.Catalog().complete = false;
        builder.Diagnose(Warning("OBJECT_RESOURCE_STANDARD_MISSING", "原版物体目录未加载；只列可确定的地图资源字段。"));
    }
    for (const auto& source : context.names.objects) {
        if (!HasKnownSource(context, source.type)) {
            builder.Catalog().complete = false;
            builder.Diagnose(Warning("OBJECT_RESOURCE_SOURCE_UNAVAILABLE", "物编覆盖状态未知，不回退原版候选：" + std::string(ObjectData::ToString(source.type))));
            continue;
        }
        if (source.state == ProjectSearchState::Partial) {
            builder.Catalog().complete = false;
            builder.Diagnose(Warning("OBJECT_RESOURCE_SOURCE_PARTIAL", "物编只部分可读：" + std::string(ObjectData::ToString(source.type))));
        }
        for (const auto& record : source.records) {
            if (!builder.Tick()) return builder.Finish();
            const auto key = std::pair{record.key.type, record.key.rawcode};
            if (objects.size() >= limits.maxObjects && !objects.contains(key)) { builder.Exhausted(); break; }
            objects.insert(key);
            const auto [found, inserted] = records.emplace(key, RecordSource{&record, source.origin});
            if (!inserted) found->second.ambiguous = true;
        }
    }
    for (const auto& [type, rawcode] : objects) {
        if (!builder.Tick()) break;
        if (!HasKnownSource(context, type)) continue;
        const ObjectKey key{type, rawcode};
        EffectiveFields fields;
        if (!CaptureFields(context, records, key, fields, builder)) continue;
        const auto name = ResolveObjectName(context.names, key);
        const auto title = name.displayName.empty() ? rawcode : name.displayName;
        const bool custom = records.contains({type, rawcode});
        for (const auto& [address, field] : fields) {
            if (!builder.Tick()) break;
            const auto spec = DescribeObjectResourceField(*field.definition);
            const auto values = Values(field.value, spec->orderedList);
            for (std::size_t index = 0; index < values.size(); ++index) {
                const auto& value = values[index];
                if (value.empty() || value == "_" || value == "-") continue;
                ObjectResourceCandidate row;
                row.id = "object:" + std::string(ToString(type)) + ':' + rawcode + ':' + address + ':' + std::to_string(index);
                row.title = title; row.value = value; row.kind = spec->kind; row.object = key; row.customObject = custom;
                row.fieldId = field.definition->fieldId; row.sourceId = custom ? "map-object" : "standard-object";
                row.state = "unverified";
                if (spec->storesPath) row.previewPath = value;
                if (!builder.Add(std::move(row))) break;
            }
        }
    }
    return builder.Finish();
}
} // namespace War3Map::Services
