#include "War3MapCore/ObjectData/ObjectImport.h"

#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"

#include <algorithm>
#include <bit>
#include <map>
#include <set>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace War3Map::ObjectData {

Result<std::vector<std::byte>> ConvertNativeObjectDataToLni(
    ObjectType type, std::span<const std::byte> nativeBytes) {
    NativeObjectCodec codec(type);
    CodecParseResult parsed = codec.Parse(nativeBytes);
    const bool parsedSuccessfully = parsed.Succeeded();

    Result<std::vector<std::byte>> result;
    result.diagnostics = std::move(parsed.diagnostics);

    if (!parsedSuccessfully || parsed.document == nullptr) {
        if (result.diagnostics.empty()) {
            result.diagnostics.push_back(Diagnostic{
                DiagnosticSeverity::Error,
                "ObjectData.Import",
                "native-parse-failed",
                "native 物体数据解析失败。"});
        }
        return result;
    }

    auto* nativeDoc = dynamic_cast<NativeObjectDocument*>(parsed.document.get());
    if (nativeDoc == nullptr) {
        result.diagnostics.push_back(Diagnostic{
            DiagnosticSeverity::Error,
            "ObjectData.Import",
            "native-document-type-mismatch",
            "解析结果不是 NativeObjectDocument。"});
        return result;
    }

    auto rendered = RenderObjectRecordsToLni(type, nativeDoc->Objects());
    result.diagnostics.insert(
        result.diagnostics.end(),
        rendered.diagnostics.begin(),
        rendered.diagnostics.end());

    if (!rendered.Succeeded() || !rendered.value.has_value()) {
        if (rendered.diagnostics.empty()) {
            result.diagnostics.push_back(Diagnostic{
                DiagnosticSeverity::Error,
                "ObjectData.Import",
                "lni-render-failed",
                "native 物体数据渲染为 LNI 失败。"});
        }
        return result;
    }

    result.value = std::move(*rendered.value);
    return result;
}



namespace {

ModificationKey NativeAddress(ObjectType type, ModificationKey key, std::int32_t version = 2) {
    if (type == ObjectType::Doodad && version != 1) {
        if (key.level.value_or(0) == 0) key.level.reset();
        key.variation = key.variation.value_or(0);
        key.dataIndex = key.dataIndex.value_or(0);
    } else if (type == ObjectType::Ability || type == ObjectType::Upgrade) {
        if (key.variation.value_or(0) == 0) key.variation.reset();
        key.level = key.level.value_or(0);
        key.dataIndex = key.dataIndex.value_or(0);
    } else {
        if (key.level.value_or(0) == 0) key.level.reset();
        if (key.dataIndex.value_or(0) == 0) key.dataIndex.reset();
        if (key.variation.value_or(0) == 0) key.variation.reset();
    }
    return key;
}

// 按格式对齐 LNI 占位索引，包含本次新增对象；非零非法索引留给写入器拒绝。
void AlignLevelPlaceholders(
    const NativeObjectDocument& nativeDocument,
    std::vector<ObjectRecord>& lniObjects) {
    for (auto& lniObject : lniObjects) {
        for (auto& modification : lniObject.modifications) {
            modification.key = NativeAddress(nativeDocument.Type(), std::move(modification.key), nativeDocument.Version());
        }
    }
}

// 从 LNI 值推断 native 值类型（仅用于新增字段）。
// 注意：float 一律推断为 Real——Unreal 无法从文本区分，新增技能等级字段时需知悉。
[[nodiscard]] NativeValueType InferNativeValueType(const Value& value) {
    if (std::holds_alternative<std::int32_t>(value)) {
        return NativeValueType::Int32;
    }
    if (std::holds_alternative<float>(value)) {
        return NativeValueType::Real;
    }
    return NativeValueType::String;
}

// 新增字段的类型策略只有一处；预算与真正 Added 写入保持一致，已有字段不走此推断。
[[nodiscard]] NativeValueType NewNativeValueType(ObjectType type, const ModificationKey& key,
    const Value& value, const ObjectFieldSchema* schema) {
    auto valueType = InferNativeValueType(value);
    const auto* definition = schema ? schema->FindNative(type, key.fieldId) : nullptr;
    if (definition && definition->nativeValueType) valueType = *definition->nativeValueType;
    return valueType;
}

// 只生成 native 写出视图。LNI 文档仍保留用户的多层父链，以供工程编辑和撤销。
[[nodiscard]] Result<std::vector<ObjectRecord>> FlattenCustomParents(
    const std::vector<ObjectRecord>& source,
    const StandardObjectCatalog* catalog,
    const ObjectFieldSchema* schema) {
    using Records = std::vector<ObjectRecord>;
    using FieldIdentity = std::tuple<std::string, std::optional<std::uint32_t>,
        std::optional<std::uint32_t>, std::optional<std::uint32_t>>;
    std::unordered_map<std::string, std::size_t> byId;
    for (std::size_t index = 0; index < source.size(); ++index) {
        if (!byId.emplace(source[index].key.rawcode, index).second) {
            return Result<Records>::Failure("ObjectData.Import", "lni-parent-ambiguous",
                "物编文档中有重复对象 ID，无法确定继承父对象: " + source[index].key.rawcode);
        }
    }

    Records flattened = source;
    for (std::size_t index = 0; index < source.size(); ++index) {
        const ObjectRecord& child = source[index];
        if (!child.parentRawcode) continue;
        if (*child.parentRawcode == child.key.rawcode) {
            return Result<Records>::Failure("ObjectData.Import", "lni-parent-cycle",
                "对象 " + child.key.rawcode + " 的父 ID 指向自身。");
        }
        if (catalog && catalog->Find(child.key.type, child.key.rawcode)) {
            return Result<Records>::Failure("ObjectData.Import", "lni-parent-standard-id-conflict",
                "自定义对象 ID 与原版目录冲突: " + child.key.rawcode);
        }
        const auto directParent = byId.find(*child.parentRawcode);
        if (directParent == byId.end() || !source[directParent->second].parentRawcode) {
            // 单层对象维持既有编码；有目录时仍须确认父级确实是原版对象。
            if (catalog && !catalog->Find(child.key.type, *child.parentRawcode)) {
                return Result<Records>::Failure("ObjectData.Import", "lni-parent-base-missing",
                    "对象 " + child.key.rawcode + " 的父 ID 不在原版目录: " + *child.parentRawcode);
            }
            continue;
        }
        if (catalog == nullptr) {
            return Result<Records>::Failure("ObjectData.Import", "lni-parent-catalog-missing",
                "对象 " + child.key.rawcode + " 继承地图自定义对象，缺少原版目录以确认最终基础 ID。");
        }
        std::unordered_set<std::string> visited;
        std::vector<const ObjectRecord*> chain;
        const ObjectRecord* current = &child;
        std::string baseId;
        while (current != nullptr) {
            if (!visited.insert(current->key.rawcode).second) {
                return Result<Records>::Failure("ObjectData.Import", "lni-parent-cycle",
                    "对象 " + child.key.rawcode + " 的自定义父链存在循环: " + current->key.rawcode);
            }
            if (current->parentRawcode && catalog->Find(child.key.type, current->key.rawcode)) {
                return Result<Records>::Failure("ObjectData.Import", "lni-parent-standard-id-conflict",
                    "自定义对象 ID 与原版目录冲突: " + current->key.rawcode);
            }
            chain.push_back(current);
            const std::string nextId = current->parentRawcode.value_or(current->key.rawcode);
            if (nextId == current->key.rawcode && current->parentRawcode) {
                return Result<Records>::Failure("ObjectData.Import", "lni-parent-cycle",
                    "对象 " + child.key.rawcode + " 的父 ID 指向自身。");
            }
            const auto next = byId.find(nextId);
            if (next != byId.end() && next->second != byId.at(current->key.rawcode)) {
                current = &source[next->second];
                continue;
            }
            if (catalog->Find(child.key.type, nextId) == nullptr) {
                return Result<Records>::Failure("ObjectData.Import", "lni-parent-base-missing",
                    "对象 " + child.key.rawcode + " 的自定义父链缺少原版终点: " + nextId);
            }
            baseId = nextId;
            break;
        }

        std::vector<ObjectModification> effective;
        std::unordered_map<std::string, NativeValueType> fieldTypes;
        for (auto owner = chain.rbegin(); owner != chain.rend(); ++owner) {
            std::set<FieldIdentity> localKeys;
            for (const ObjectModification& field : (*owner)->modifications) {
                const auto& key = field.key;
                const FieldIdentity identity{key.fieldId, key.level, key.dataIndex, key.variation};
                if (!localKeys.insert(identity).second) {
                    return Result<Records>::Failure("ObjectData.Import", "lni-parent-field-alias",
                        "对象 " + (*owner)->key.rawcode + " 有多个字段映射到同一原生地址: " + key.fieldId);
                }
                if (field.valueWasOpaque || std::holds_alternative<UnknownBytesValue>(field.value)) {
                    return Result<Records>::Failure("ObjectData.Import", "lni-parent-opaque-field",
                        "对象 " + (*owner)->key.rawcode + " 的字段 " + key.fieldId + " 无法安全继承写出。");
                }
                if (key.fieldId.size() != 4 || key.fieldId.find('\0') != std::string::npos ||
                    std::holds_alternative<bool>(field.value) || std::holds_alternative<RawcodeListValue>(field.value)) {
                    return Result<Records>::Failure("ObjectData.Import", "lni-parent-field-unencodable",
                        "对象 " + (*owner)->key.rawcode + " 的字段 " + key.fieldId + " 无法编码为原生物编值。");
                }
                NativeValueType nativeType = InferNativeValueType(field.value);
                if (std::holds_alternative<float>(field.value)) {
                    const auto* definition = schema ? schema->FindNative(child.key.type, key.fieldId) : nullptr;
                    if (definition == nullptr || !definition->nativeValueType ||
                        (*definition->nativeValueType != NativeValueType::Real &&
                            *definition->nativeValueType != NativeValueType::Unreal)) {
                        return Result<Records>::Failure("ObjectData.Import", "lni-parent-float-type-unknown",
                            "字段 " + key.fieldId + " 的 Real/Unreal 存储类型无法确认，未猜测继承值。");
                    }
                    nativeType = *definition->nativeValueType;
                }
                const auto fieldAddress = key.fieldId + '\0' +
                    std::to_string(key.level.value_or(0)) + '/' +
                    std::to_string(key.dataIndex.value_or(0)) + '/' +
                    std::to_string(key.variation.value_or(0));
                const auto oldType = fieldTypes.find(fieldAddress);
                if (oldType != fieldTypes.end() && oldType->second != nativeType) {
                    return Result<Records>::Failure("ObjectData.Import", "lni-parent-field-type-conflict",
                        "字段 " + key.fieldId + " 在继承链上使用了不同的原生值类型。");
                }
                fieldTypes[fieldAddress] = nativeType;
                const auto existing = std::find_if(effective.begin(), effective.end(),
                    [&](const ObjectModification& candidate) { return candidate.key == key; });
                if (existing == effective.end()) effective.push_back(field);
                else *existing = field; // 越靠近子对象的明确覆盖优先。
            }
        }
        flattened[index].parentRawcode = std::move(baseId);
        flattened[index].modifications = std::move(effective);
    }
    return Result<Records>::Success(std::move(flattened));
}

Status ApplyLniChangesToNativeStaged(
    NativeObjectDocument& nativeDocument,
    std::span<const ObjectRecord> sourceObjects,
    const ObjectFieldSchema* schema) {
    // 构造比较视图：不透明字段（多行文本等只读值）在两侧都剔除——
    // 它们不可编辑，写回时保持 native 原值，不参与差异计算。
    std::vector<ObjectRecord> nativeView(
        nativeDocument.Objects().begin(), nativeDocument.Objects().end());
    std::vector<ObjectRecord> lniObjects(
        sourceObjects.begin(), sourceObjects.end());
    // 先消除 LNI 为 variation 补出的 level=0 等占位，才能精确匹配只读字段。
    AlignLevelPlaceholders(nativeDocument, lniObjects);
    for (auto& lniObject : lniObjects) {
        using FieldIdentity = std::tuple<std::string, std::optional<std::uint32_t>,
            std::optional<std::uint32_t>, std::optional<std::uint32_t>>;
        std::set<FieldIdentity> identities;
        for (const auto& modification : lniObject.modifications) {
            const auto& key = modification.key;
            if (!identities.emplace(key.fieldId, key.level, key.dataIndex, key.variation).second) {
                return Status::Failure("ObjectData.Import", "lni-native-field-alias",
                    "多条 LNI 字段映射到同一个 native 字段，未写入存在歧义的覆盖。");
            }
        }
        ObjectRecord* nativeObject = nullptr;
        for (auto& candidate : nativeView) {
            if (candidate.key == lniObject.key) {
                nativeObject = &candidate;
                break;
            }
        }
        if (nativeObject == nullptr) {
            if (std::any_of(lniObject.modifications.begin(), lniObject.modifications.end(),
                [](const ObjectModification& mod) { return mod.valueWasOpaque; })) {
                return Status::Failure("ObjectData.Import", "lni-new-object-opaque-field",
                    "新增对象包含无法编码的原始字段，未丢弃字段或写入部分对象。");
            }
            continue;
        }
        for (const auto& modification : lniObject.modifications) {
            if (modification.valueWasOpaque && std::none_of(
                nativeObject->modifications.begin(), nativeObject->modifications.end(),
                [&](const auto& original) { return original.key == modification.key; })) {
                return Status::Failure("ObjectData.Import", "lni-new-opaque-field",
                    "新增原始字段没有可保留的 native 来源，未忽略字段或写入部分修改。");
            }
        }
        std::erase_if(nativeObject->modifications, [&](const ObjectModification& mod) {
            return std::any_of(
                lniObject.modifications.begin(), lniObject.modifications.end(),
                [&](const ObjectModification& lniMod) {
                    return lniMod.valueWasOpaque && lniMod.key == mod.key;
                });
        });
        std::erase_if(lniObject.modifications,
            [](const ObjectModification& mod) { return mod.valueWasOpaque; });
    }
    const ObjectSemanticDiff diff = DiffObjects(nativeView, lniObjects);

    for (const ObjectChange& change : diff.changes) {
        if (!change.modification.has_value()) {
            // 对象级差异。
            if (change.kind == ObjectChangeKind::Added) {
                const auto found = std::find_if(lniObjects.begin(), lniObjects.end(),
                    [&](const auto& record) { return record.key == change.object; });
                const ObjectRecord* lniObject = found == lniObjects.end() ? nullptr : &*found;
                if (!lniObject) return Status::Failure("ObjectData.Import", "lni-object-not-found", "新增对象未出现在 LNI 差异视图中。");
                const std::string parent =
                    (lniObject != nullptr && lniObject->parentRawcode.has_value())
                    ? *lniObject->parentRawcode
                    : std::string{};
                const Status added = lniObject->parentRawcode.has_value()
                    ? nativeDocument.AddCustomObject(change.object.rawcode, parent)
                    : nativeDocument.AddOriginalObject(change.object.rawcode);
                if (!added.Succeeded()) {
                    return added;
                }
                // DiffObjects 的 Added 对象仅有对象级记录；全部初始字段需要在此显式写入。
                for (const auto& modification : lniObject->modifications) {
                    const auto valueType = NewNativeValueType(change.object.type, modification.key, modification.value, schema);
                    auto fieldAdded = nativeDocument.AddModification(change.object, modification.key, valueType, modification.value);
                    if (!fieldAdded.Succeeded()) {
                        fieldAdded.diagnostics.push_back({DiagnosticSeverity::Error, "ObjectData.Import",
                            "lni-native-create-field", "新增对象字段写回失败: " + change.object.rawcode + "/"
                                + modification.key.fieldId + "，目标存储类型="
                                + std::to_string(static_cast<std::int32_t>(valueType))
                                + "，值变体=" + std::to_string(modification.value.index())});
                        return fieldAdded;
                    }
                }
                continue;
            }
            if (change.kind == ObjectChangeKind::Removed) {
                const auto* previous = nativeDocument.FindObject(change.object);
                if (!previous) return Status::Failure("ObjectData.Import", "native-object-not-found", "待清除的覆盖对象不存在。");
                const Status removed = previous->parentRawcode.has_value()
                    ? nativeDocument.DeleteCustomObject(change.object)
                    : nativeDocument.RemoveOriginalObjectOverrides(change.object);
                if (!removed.Succeeded()) {
                    return removed;
                }
                continue;
            }
            continue; // Modified 对象级仅 _parent 分支携带 modification，见下。
        }

        if (change.modification->fieldId == "_parent") {
            return Status::Failure(
                "ObjectData.Import",
                "lni-native-reparent",
                "LNI 写回暂不支持变更对象父级: " + change.object.rawcode);
        }
        if (change.kind == ObjectChangeKind::Modified && change.after.has_value()) {
            const Status modified = nativeDocument.SetModification(
                change.object, *change.modification, *change.after);
            if (!modified.Succeeded()) {
                Status contextual = modified;
                contextual.diagnostics.push_back(Diagnostic{
                    DiagnosticSeverity::Error,
                    "ObjectData.Import",
                    "lni-native-apply-field",
                    "写回字段失败: " + change.object.rawcode + "/" +
                        change.modification->fieldId});
                return contextual;
            }
            continue;
        }
        if (change.kind == ObjectChangeKind::Added && change.after.has_value()) {
            const auto valueType = NewNativeValueType(change.object.type, *change.modification, *change.after, schema);
            const Status added = nativeDocument.AddModification(
                change.object,
                *change.modification,
                valueType,
                *change.after);
            if (!added.Succeeded()) {
                return added;
            }
            continue;
        }
        if (change.kind == ObjectChangeKind::Removed) {
            const auto removed = nativeDocument.RemoveModification(change.object, *change.modification);
            if (!removed.Succeeded()) return removed;
        }
    }
    return Status::Ok();
}

struct ModificationAddressLess {
    bool operator()(const ModificationKey& left, const ModificationKey& right) const {
        return std::tie(left.fieldId, left.level, left.dataIndex, left.variation)
            < std::tie(right.fieldId, right.level, right.dataIndex, right.variation);
    }
};

struct IndexedObject {
    const ObjectRecord* record = nullptr;
    std::map<ModificationKey, const ObjectModification*, ModificationAddressLess> fields;
};

using ObjectIndex = std::map<std::string, IndexedObject>;

Result<ObjectIndex> IndexMergeObjects(ObjectType type, std::span<const ObjectRecord> records) {
    constexpr std::size_t kMaxObjects = 65536;
    constexpr std::size_t kMaxFields = 1'000'000;
    if (records.size() > kMaxObjects) {
        return Result<ObjectIndex>::Failure("ObjectData.Import", "lni-merge-limit",
            "三方合并的对象数量超过预算。");
    }
    ObjectIndex index;
    std::size_t totalFields = 0;
    for (const ObjectRecord& record : records) {
        if (record.key.type != type || record.key.rawcode.size() != 4 ||
            !index.emplace(record.key.rawcode, IndexedObject{&record, {}}).second) {
            return Result<ObjectIndex>::Failure("ObjectData.Import", "lni-merge-object-alias",
                "三方合并输入包含类型不符、非法或重复的对象 ID。");
        }
        if (record.modifications.size() > kMaxFields - totalFields) {
            return Result<ObjectIndex>::Failure("ObjectData.Import", "lni-merge-limit",
                "三方合并的字段数量超过预算。");
        }
        totalFields += record.modifications.size();
        auto& fields = index.at(record.key.rawcode).fields;
        for (const ObjectModification& field : record.modifications) {
            if (!fields.emplace(NativeAddress(type, field.key), &field).second) {
                return Result<ObjectIndex>::Failure("ObjectData.Import", "lni-merge-field-alias",
                    "多条字段映射到同一个 native 地址：" + record.key.rawcode + "/" + field.key.fieldId);
            }
        }
    }
    return Result<ObjectIndex>::Success(std::move(index));
}

const ObjectModification* IndexedField(const IndexedObject& object, const ModificationKey& key) {
    const auto found = object.fields.find(key);
    return found == object.fields.end() ? nullptr : found->second;
}

bool SameMergeValue(const ObjectModification* left, const ObjectModification* right) {
    if (left == nullptr || right == nullptr) return left == right;
    if (left->value.index() != right->value.index()) return false;
    const auto* leftFloat = std::get_if<float>(&left->value);
    if (leftFloat != nullptr) {
        return std::bit_cast<std::uint32_t>(*leftFloat) ==
            std::bit_cast<std::uint32_t>(std::get<float>(right->value));
    }
    return left->value == right->value;
}

Result<LniObjectDocument> MergeFailure(std::string code, std::string message) {
    return Result<LniObjectDocument>::Failure("ObjectData.Import", std::move(code), std::move(message));
}

Result<LniObjectDocument> MergeMutationFailure(
    std::string message, const Status& failed) {
    auto result = MergeFailure("lni-merge-mutation-rejected", std::move(message));
    result.diagnostics.insert(result.diagnostics.end(), failed.diagnostics.begin(), failed.diagnostics.end());
    return result;
}

} // namespace

Result<std::vector<std::size_t>> PlanNativeCloneProjection(
    const NativeObjectDocument& nativeDocument, const LniObjectDocument& lniDocument,
    const ObjectFieldSchema* schema) {
    using Order = std::vector<std::size_t>;
    const auto reject = [](std::string code, std::string message) {
        return Result<Order>::Failure("ObjectData.LNI", std::move(code), std::move(message));
    };
    const auto sources = lniDocument.CloneSources();
    if (sources.empty()) return Result<Order>::Success(Order{});
    if (sources.size() > LniObjectDocument::kMaximumCloneSources) {
        return reject("lni-clone-source-budget", "克隆来源记录超过预算。");
    }
    if (!lniDocument.IsStructurallySafe() || nativeDocument.Type() != lniDocument.Type()
        || !nativeDocument.Supports(NativeObjectOperation::EditStructure)) {
        return reject("lni-native-clone-unsafe", "克隆要求同类、结构完整且允许结构编辑的实际 native 基线。");
    }
    const auto validRawcode = [](std::string_view rawcode) {
        return rawcode.size() == 4 && std::all_of(rawcode.begin(), rawcode.end(), [](unsigned char value) {
            return value >= 0x21 && value <= 0x7e && value != '[' && value != ']';
        });
    };
    const auto opaque = [](const ObjectRecord& record) {
        return std::any_of(record.modifications.begin(), record.modifications.end(), [](const auto& field) {
            return field.valueWasOpaque || std::holds_alternative<UnknownBytesValue>(field.value);
        });
    };
    const auto type = lniDocument.Type();
    std::unordered_map<std::string, std::size_t> byDestination;
    for (std::size_t index = 0; index < sources.size(); ++index) {
        const auto& source = sources[index];
        if (!validRawcode(source.newRawcode) || !validRawcode(source.sourceRawcode)
            || source.newRawcode == source.sourceRawcode
            || !byDestination.emplace(source.newRawcode, index).second) {
            return reject("lni-native-clone-identity", "克隆来源含无效 ID、自引用或重复目标。");
        }
        const auto* target = lniDocument.FindObject({type, source.newRawcode});
        const auto* original = lniDocument.FindObject({type, source.sourceRawcode});
        if (!target || !original || !target->parentRawcode || !original->parentRawcode
            || target->parentRawcode != original->parentRawcode) {
            return reject("lni-native-clone-parent", "克隆来源或目标缺失、不是自定义对象，或父 ID 不同: "
                + source.sourceRawcode + " -> " + source.newRawcode);
        }
        if (opaque(*target) || opaque(*original)) {
            return reject("lni-native-clone-opaque", "克隆含无法编码的原始字段，未放宽新增对象的 opaque 门禁。");
        }
        if (nativeDocument.FindObject({type, source.newRawcode})) {
            return reject("lni-native-clone-target-exists", "克隆目标已在 native 基线中，不能覆盖已有对象: " + source.newRawcode);
        }
    }
    std::vector<Order> dependents(sources.size());
    Order ready;
    ready.reserve(sources.size());
    for (std::size_t index = 0; index < sources.size(); ++index) {
        const auto& source = sources[index];
        if (const auto dependency = byDestination.find(source.sourceRawcode); dependency != byDestination.end()) {
            dependents[dependency->second].push_back(index);
            continue;
        }
        const auto* original = nativeDocument.FindObject({type, source.sourceRawcode});
        if (original) {
            const auto* editable = lniDocument.FindObject({type, source.sourceRawcode});
            if (!original->parentRawcode) {
                return reject("lni-native-clone-source-missing", "克隆链没有实际 native 自定义来源: " + source.sourceRawcode);
            }
            if (original->parentRawcode != editable->parentRawcode) {
                return reject("lni-native-clone-parent", "克隆来源的 native 父 ID 与 LNI 不同: " + source.sourceRawcode);
            }
            if (opaque(*original)) {
                return reject("lni-native-clone-opaque", "实际 native 来源含无法安全复制的原始字段。");
            }
        }
        // 不在 native 中的根已由上面的 LNI 身份校验确认存在；Stage 会按 Added 规则严格编码。
        ready.push_back(index);
    }
    // 只排序小的来源索引，不物化 clone-of-clone，长链也不会递归消耗调用栈。
    for (std::size_t index = 0; index < ready.size(); ++index) {
        ready.insert(ready.end(), dependents[ready[index]].begin(), dependents[ready[index]].end());
    }
    if (ready.size() != sources.size()) {
        return reject("lni-native-clone-cycle", "克隆来源链存在循环，未创建部分 native 对象。");
    }
    constexpr auto limit = LniObjectDocument::kMaximumNativeCloneProjectionBytes;
    const auto baselineBytes = nativeDocument.SerializedByteSize();
    if (!baselineBytes) {
        return reject("lni-native-clone-size-unknown", "无法只读确认实际 native wire 大小，未物化克隆。");
    }
    const auto overBudget = [&] {
        return reject("lni-native-clone-projection-budget",
            "克隆物化阶段的 native wire 文档将超过 64 MiB 预算，未创建普通新根或克隆对象。");
    };
    if (*baselineBytes > limit) return overBudget();
    std::size_t projectionBytes = *baselineBytes;
    const auto addProjection = [&projectionBytes](std::size_t bytes) {
        if (bytes > limit - projectionBytes) return false;
        projectionBytes += bytes;
        return true;
    };
    // 此处仅处理普通新根：类型选择沿用既有 Added-object 策略；已有来源始终读实际 storage。
    const auto newFieldBytes = [schema, type](const ObjectModification& field) -> std::optional<std::size_t> {
        if (field.valueWasOpaque || std::holds_alternative<UnknownBytesValue>(field.value)) return std::nullopt;
        const auto valueType = NewNativeValueType(type, field.key, field.value, schema);
        const std::size_t headerBytes = type == ObjectType::Ability || type == ObjectType::Upgrade
            || type == ObjectType::Doodad ? 20 : 12;
        if (valueType == NativeValueType::Int32) {
            return std::holds_alternative<std::int32_t>(field.value)
                ? std::optional<std::size_t>{headerBytes + 4} : std::nullopt;
        }
        if (valueType == NativeValueType::Real || valueType == NativeValueType::Unreal) {
            return std::holds_alternative<float>(field.value) || std::holds_alternative<std::int32_t>(field.value)
                ? std::optional<std::size_t>{headerBytes + 4} : std::nullopt;
        }
        if (valueType != NativeValueType::String) return std::nullopt;
        const auto text = std::visit([](const auto& value) -> std::optional<std::string_view> {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::string>) return value;
            else if constexpr (std::is_same_v<T, RawcodeValue> || std::is_same_v<T, EnumValue>
                || std::is_same_v<T, AssetPathValue> || std::is_same_v<T, LocalizedStringValue>) return value.value;
            else return std::nullopt;
        }, field.value);
        if (!text || text->find('\0') != std::string_view::npos) return std::nullopt;
        // 用预算作加法上界，避免字符串长度加 header 或 NUL 时溢出。
        if (text->size() > limit - headerBytes - 1) return limit + 1;
        return headerBytes + text->size() + 1;
    };
    std::unordered_map<std::string, std::size_t> recordBytes;
    for (const auto index : ready) {
        const auto& source = sources[index];
        auto root = recordBytes.find(source.sourceRawcode);
        if (root == recordBytes.end()) {
            std::size_t bytes = 0;
            if (nativeDocument.FindObject({type, source.sourceRawcode})) {
                const auto actualBytes = nativeDocument.ObjectSerializedByteSize({type, source.sourceRawcode});
                if (!actualBytes) return reject("lni-native-clone-size-unknown", "无法确认实际克隆来源的 wire 大小。");
                bytes = *actualBytes;
            } else {
                bytes = 12;
                const auto* current = lniDocument.FindObject({type, source.sourceRawcode});
                std::unordered_set<std::string> visited;
                bool inheritsCustom = false;
                while (current) {
                    if (!visited.insert(current->key.rawcode).second) {
                        return reject("lni-parent-cycle", "普通新克隆根的自定义父链存在循环。");
                    }
                    for (const auto& field : current->modifications) {
                        const auto size = newFieldBytes(field);
                        if (!size) return reject("lni-native-clone-pending-field", "普通新克隆根含不能按既有 Added 规则编码的字段。");
                        if (*size > limit - bytes) return overBudget();
                        bytes += *size;
                    }
                    const auto* parent = current->parentRawcode
                        ? lniDocument.FindObject({type, *current->parentRawcode}) : nullptr;
                    if (!parent || (!inheritsCustom && !parent->parentRawcode)) break;
                    inheritsCustom = true;
                    current = parent;
                }
                // 每个普通新根只计一次。父链字段全加、忽略覆盖消减，形成展平前可读的保守上界。
                if (!addProjection(bytes)) return overBudget();
            }
            root = recordBytes.emplace(source.sourceRawcode, bytes).first;
        }
        const auto bytes = root->second;
        // 克隆时只改等长 ID/结束标记，记录大小相同；最终 LNI 删除字段不能抵消此峰值。
        if (!addProjection(bytes)) return overBudget();
        recordBytes.emplace(source.newRawcode, bytes);
    }
    return Result<Order>::Success(std::move(ready));
}

Status StagePendingNativeCloneSources(
    NativeObjectDocument& nativeDocument, const LniObjectDocument& lniDocument,
    const ObjectFieldSchema* schema, const StandardObjectCatalog* standardCatalog) {
    const auto sources = lniDocument.CloneSources();
    if (sources.empty()) return Status::Ok();
    const auto plan = PlanNativeCloneProjection(nativeDocument, lniDocument, schema);
    if (!plan.Succeeded()) { Status failed; failed.diagnostics = plan.diagnostics; return failed; }
    std::set<std::string> destinations, pendingRoots;
    for (const auto& source : sources) destinations.insert(source.newRawcode);
    for (const auto& source : sources) {
        if (destinations.contains(source.sourceRawcode)
            || nativeDocument.FindObject({lniDocument.Type(), source.sourceRawcode})) continue;
        pendingRoots.insert(source.sourceRawcode);
    }
    if (pendingRoots.empty()) return Status::Ok();
    const std::vector<ObjectRecord> editable(lniDocument.Objects().begin(), lniDocument.Objects().end());
    auto flattened = FlattenCustomParents(editable, standardCatalog, schema);
    if (!flattened.Succeeded() || !flattened.value) {
        Status failed; failed.diagnostics = std::move(flattened.diagnostics); return failed;
    }
    // 已有来源仍取原 native 记录。只加入普通新根，再由现有 Added-object 分支
    // 校验每个字段；因此旧 E000 的真实 wire 类型不会先被当前 LNI 编辑重写。
    std::vector<ObjectRecord> sourceView(nativeDocument.Objects().begin(), nativeDocument.Objects().end());
    for (const auto& record : *flattened.value)
        if (pendingRoots.contains(record.key.rawcode)) sourceView.push_back(record);
    NativeObjectDocument candidate = nativeDocument;
    const auto applied = ApplyLniChangesToNativeStaged(candidate, sourceView, schema);
    if (!applied.Succeeded()) return applied;
    nativeDocument = std::move(candidate);
    return Status::Ok();
}

Status ApplyLniChangesToNative(
    NativeObjectDocument& nativeDocument,
    const LniObjectDocument& lniDocument,
    const ObjectFieldSchema* schema,
    const StandardObjectCatalog* standardCatalog) {
    if (nativeDocument.Type() != lniDocument.Type()) {
        return Status::Failure("ObjectData.Import", "lni-native-type-mismatch", "LNI 与 native 文档的物体类型不一致。");
    }
    if (!nativeDocument.Supports(NativeObjectOperation::EditExistingValue) || !lniDocument.IsStructurallySafe()) {
        return Status::Failure("ObjectData.Import", "lni-native-document-unsafe", "写回要求两侧文档结构完整且 native 文档允许已有字段值编辑。");
    }
    const auto plan = PlanNativeCloneProjection(nativeDocument, lniDocument, schema);
    if (!plan.Succeeded()) { Status failed; failed.diagnostics = plan.diagnostics; return failed; }
    // 保留全部来源字节及内部索引的隔离副本；任何后续失败均不会污染调用方。
    NativeObjectDocument staged = nativeDocument;
    std::vector<ObjectRecord> sourceObjects(lniDocument.Objects().begin(), lniDocument.Objects().end());
    AlignLevelPlaceholders(nativeDocument, sourceObjects);
    // v1 不展平父链或补继承字段，否则值编辑会暗中改变源结构。
    auto flattened = nativeDocument.Supports(NativeObjectOperation::EditStructure)
        ? FlattenCustomParents(sourceObjects, standardCatalog, schema)
        : Result<std::vector<ObjectRecord>>::Success(sourceObjects);
    if (!flattened.Succeeded() || !flattened.value) {
        Status failed;
        failed.diagnostics = std::move(flattened.diagnostics);
        return failed;
    }
    for (std::size_t index = 0; index < sourceObjects.size(); ++index) {
        if (sourceObjects[index].parentRawcode == (*flattened.value)[index].parentRawcode) continue;
        const ObjectRecord* previous = nativeDocument.FindObject(sourceObjects[index].key);
        if (previous && previous->parentRawcode != (*flattened.value)[index].parentRawcode) {
            return Status::Failure("ObjectData.Import", "lni-existing-parent-unflattened",
                "对象 " + sourceObjects[index].key.rawcode +
                " 的现有 native 父 ID 仍指向地图自定义对象；当前无无损重定父方法，未修改原有字段。");
        }
    }
    const auto sourcesStaged = StagePendingNativeCloneSources(staged, lniDocument, schema, standardCatalog);
    if (!sourcesStaged.Succeeded()) return sourcesStaged;
    const auto cloned = lniDocument.PrepareNativeClones(staged);
    if (!cloned.Succeeded()) return cloned;
    const auto applied = ApplyLniChangesToNativeStaged(staged, *flattened.value, schema);
    if (!applied.Succeeded()) return applied;
    nativeDocument = std::move(staged);
    return applied;
}

Result<LniObjectDocument> MergeGeneratedNativeWithLniEdits(
    const NativeObjectDocument& nativeBaseline,
    const LniObjectDocument& currentEditable,
    const NativeObjectDocument& generatedNative,
    const ObjectFieldSchema* schema,
    const StandardObjectCatalog* standardCatalog) {
    constexpr std::size_t kMaxLniBytes = 64 * 1024 * 1024;
    const ObjectType type = nativeBaseline.Type();
    if (currentEditable.Type() != type || generatedNative.Type() != type) {
        return MergeFailure("lni-merge-type-mismatch", "旧 native、当前 LNI 与生成 native 的物编类别不一致。");
    }
    if (!nativeBaseline.Supports(NativeObjectOperation::ReplaceFromTemplate) ||
        !currentEditable.IsStructurallySafe() ||
        !generatedNative.Supports(NativeObjectOperation::ReplaceFromTemplate)) {
        return MergeFailure("lni-merge-document-unsafe", "三方合并要求两个可编辑经典 v2 native 和结构安全的 LNI。");
    }
    const NativeObjectCodec nativeCodec(type);
    const auto generatedBytes = nativeCodec.Serialize(generatedNative);
    if (!generatedBytes.Succeeded() || !generatedBytes.value) {
        return MergeFailure("lni-merge-generated-invalid", "生成物编不能无损序列化，未合并编辑状态。");
    }
    const LniObjectCodec lniCodec(type);
    const auto originalText = lniCodec.Serialize(currentEditable);
    if (!originalText.Succeeded() || !originalText.value || originalText.value->size() > kMaxLniBytes) {
        return MergeFailure("lni-merge-text-invalid", "当前 LNI 无法无损序列化或超过 64 MiB 预算。");
    }

    auto baseline = IndexMergeObjects(type, nativeBaseline.Objects());
    auto current = IndexMergeObjects(type, currentEditable.Objects());
    auto generated = IndexMergeObjects(type, generatedNative.Objects());
    if (!baseline.Succeeded()) {
        Result<LniObjectDocument> failed; failed.diagnostics = std::move(baseline.diagnostics); return failed;
    }
    if (!current.Succeeded()) {
        Result<LniObjectDocument> failed; failed.diagnostics = std::move(current.diagnostics); return failed;
    }
    if (!generated.Succeeded()) {
        Result<LniObjectDocument> failed; failed.diagnostics = std::move(generated.diagnostics); return failed;
    }

    LniObjectDocument merged = currentEditable;
    for (const auto& [rawcode, generatedObject] : *generated.value) {
        const auto old = baseline.value->find(rawcode);
        const auto ours = current.value->find(rawcode);
        if (old == baseline.value->end()) {
            if (ours != current.value->end()) continue; // 相同新增对象留给最终 native 字节校验。
            const auto& record = *generatedObject.record;
            const Status added = record.parentRawcode
                ? merged.AddObject(rawcode, *record.parentRawcode)
                : merged.AddOriginalObject(rawcode);
            if (!added.Succeeded()) {
                return MergeMutationFailure("生成侧新增对象不能合入当前 LNI：" + rawcode, added);
            }
            for (const auto& field : record.modifications) {
                const Status set = merged.SetModification(record.key, field.key, field.value);
                if (!set.Succeeded()) {
                    return MergeMutationFailure("生成侧新增字段不能合入当前 LNI：" +
                        rawcode + "/" + field.key.fieldId, set);
                }
            }
            continue;
        }
        if (ours == current.value->end()) {
            return MergeFailure("lni-merge-object-conflict",
                "当前 LNI 已移除生成侧仍存在的对象：" + rawcode);
        }
        if (old->second.record->parentRawcode != generatedObject.record->parentRawcode &&
            ours->second.record->parentRawcode != generatedObject.record->parentRawcode) {
            return MergeFailure("lni-merge-parent-conflict",
                "生成侧改变对象父级，当前 LNI 无法无损保留：" + rawcode);
        }
        std::set<ModificationKey, ModificationAddressLess> addresses;
        for (const auto& field : old->second.fields) addresses.insert(field.first);
        for (const auto& field : generatedObject.fields) addresses.insert(field.first);
        for (const ModificationKey& address : addresses) {
            const auto* before = IndexedField(old->second, address);
            const auto* theirs = IndexedField(generatedObject, address);
            if (SameMergeValue(before, theirs)) continue;
            const auto* oursField = IndexedField(ours->second, address);
            if (oursField && oursField->valueWasOpaque) {
                return MergeFailure("lni-merge-opaque-conflict",
                    "生成侧修改了当前 LNI 中语义不透明的字段：" + rawcode + "/" + address.fieldId);
            }
            if (SameMergeValue(oursField, theirs)) continue;
            if (!SameMergeValue(oursField, before)) {
                return MergeFailure("lni-merge-field-conflict",
                    "当前 LNI 与生成侧修改了同一字段且结果不同：" + rawcode + "/" + address.fieldId);
            }
            const auto& key = oursField ? oursField->key : theirs->key;
            const Status changed = theirs
                ? merged.SetModification(generatedObject.record->key, key, theirs->value)
                : merged.RemoveModification(generatedObject.record->key, key);
            if (!changed.Succeeded()) {
                return MergeMutationFailure("生成侧字段变动不能合入当前 LNI：" +
                    rawcode + "/" + address.fieldId, changed);
            }
        }
    }
    for (const auto& entry : *baseline.value) {
        const auto& rawcode = entry.first;
        if (generated.value->contains(rawcode)) continue;
        if (current.value->contains(rawcode)) {
            return MergeFailure("lni-merge-object-removal",
                "生成侧移除的对象仍在当前 LNI；没有可证明安全的删除审计：" + rawcode);
        }
    }

    const auto mergedText = lniCodec.Serialize(merged);
    if (!mergedText.Succeeded() || !mergedText.value || mergedText.value->size() > kMaxLniBytes) {
        return MergeFailure("lni-merge-text-invalid", "合并后的 LNI 无法无损序列化或超过 64 MiB 预算。");
    }
    auto reparsed = lniCodec.Parse(*mergedText.value);
    auto* reopened = reparsed.Succeeded() && reparsed.document
        ? dynamic_cast<LniObjectDocument*>(reparsed.document.get()) : nullptr;
    if (!reopened || !reopened->IsStructurallySafe()) {
        return MergeFailure("lni-merge-text-roundtrip", "合并后的 LNI 无法重新解析，未替换编辑基线。");
    }
    const auto cloneSources = reopened->RestoreCloneSources(currentEditable.CloneSources(), nativeBaseline, schema, standardCatalog);
    if (!cloneSources.Succeeded()) {
        auto failed = MergeFailure("lni-merge-clone-source-rejected", "合并后的克隆来源不能从实际 native 基线恢复。");
        failed.diagnostics.insert(failed.diagnostics.end(), cloneSources.diagnostics.begin(), cloneSources.diagnostics.end());
        return failed;
    }
    NativeObjectDocument projection = nativeBaseline;
    const Status projected = ApplyLniChangesToNative(projection, *reopened, schema, standardCatalog);
    if (!projected.Succeeded()) {
        auto failed = MergeFailure("lni-merge-projection-rejected",
            "合并后的编辑文档不能完整投影为 native，未替换编辑基线。");
        failed.diagnostics.insert(failed.diagnostics.end(),
            projected.diagnostics.begin(), projected.diagnostics.end());
        return failed;
    }
    const auto projectedBytes = nativeCodec.Serialize(projection);
    if (!projectedBytes.Succeeded() || !projectedBytes.value ||
        *projectedBytes.value != *generatedBytes.value) {
        return MergeFailure("lni-merge-native-mismatch",
            "合并 LNI 投影与模板生成的完整 native 成员字节不同，未丢弃任一来源语义。");
    }
    return Result<LniObjectDocument>::Success(std::move(*reopened));
}


} // namespace War3Map::ObjectData
