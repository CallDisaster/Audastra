#include "SlkMaterializerInternal.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <set>

// 独立实现。目标 frontend_obj:24 去掉字段ID尾NUL；updateobj:29–82 先common后byCode；
// frontend_merge:20–157 保留全部默认字段并优先byCode选择fill规则。没有复制其实现。
namespace War3Map::Script::MaterializerDetail {
namespace {
const SlkSessionScalar* MetaScalar(const SlkDatasetFields& fields, std::string_view key) {
    const auto at = fields.find(std::string(key));
    return at == fields.end() ? nullptr : std::get_if<SlkSessionScalar>(&at->second.value);
}
const std::string* MetaString(const SlkDatasetFields& fields, std::string_view key) {
    const auto* scalar = MetaScalar(fields, key); return scalar ? std::get_if<std::string>(scalar) : nullptr;
}
OriginSpan Where(const SlkTypedDataset& dataset, const SlkDatasetPosition& position, const OriginSpan& fallback) {
    if (position.sourceIndex >= dataset.sources.size()) Reject("SLK_MATERIALIZER_IDENTITY_MISMATCH", "canonical字段来源索引越界。", fallback);
    const auto& source = dataset.sources[position.sourceIndex];
    if (position.offset > source.byteCount || position.length > source.byteCount - position.offset)
        Reject("SLK_MATERIALIZER_IDENTITY_MISMATCH", "canonical字段来源范围越界。", fallback);
    return {source.path, position.offset, position.length, source.sha256};
}
bool Flag(const SlkDatasetFields& fields, std::string_view key, const OriginSpan& origin) {
    if (!fields.contains(std::string(key))) return false;
    const auto* scalar = MetaScalar(fields, key);
    const auto* boolean = scalar ? std::get_if<bool>(scalar) : nullptr;
    if (!boolean) Reject("SLK_MATERIALIZER_METADATA_UNSUPPORTED", "canonical标志必须是boolean。", origin);
    return *boolean;
}
std::int64_t Integer(const SlkDatasetFields& fields, std::string_view key, bool required, const OriginSpan& origin) {
    if (!required && !fields.contains(std::string(key))) return 0;
    const auto* scalar = MetaScalar(fields, key);
    const auto* number = scalar ? std::get_if<std::int64_t>(scalar) : nullptr;
    if (!number || *number < 0 || *number > 256) Reject("SLK_MATERIALIZER_METADATA_UNSUPPORTED", "canonical索引/类型必须是有界整数。", origin);
    return *number;
}
struct Rule {
    std::string id;
    NativeValueType nativeType = NativeValueType::Int32;
    std::uint32_t data = 0;
    bool repeated = false, profile = false, append = false, split = false;
};
using Rules = std::map<std::string, Rule>;
Rules ReadRules(const SlkTypedDataset& dataset, const SlkDatasetMetadataScope& scope, Budget& budget, const OriginSpan& origin) {
    Rules rules;
    constexpr std::string_view allowed[]{"appendindex", "cantempty", "concat", "data", "default", "displayname", "field", "id", "index", "key", "profile", "repeat", "splite", "type"};
    for (const auto& [name, record] : scope.fields) {
        budget.Spend(budget.metadataVisits, 1, 50000000, origin);
        budget.FieldKey(name, origin);
        const auto where = Where(dataset, record.source, origin);
        const auto& properties = record.fields;
        const auto* id = MetaString(properties, "id");
        const auto* field = MetaString(properties, "field");
        const auto* key = MetaString(properties, "key");
        const auto type = Integer(properties, "type", true, where);
        if (!id || id->empty() || id->size() > 4 || !std::all_of(id->begin(), id->end(), [](unsigned char c) { return c >= 0x21 && c <= 0x7e; }) ||
            !field || Text::LowerAscii(*field) != name || !key || type > 3 || name.empty() || name.front() == '_')
            Reject("SLK_MATERIALIZER_METADATA_UNSUPPORTED", "canonical字段身份或类型无效。", where);
        for (const auto& [property, value] : properties) {
            (void)value;
            if (std::find(std::begin(allowed), std::end(allowed), property) == std::end(allowed))
                Reject("SLK_MATERIALIZER_METADATA_UNSUPPORTED", "canonical字段含未知metadata属性。", where);
        }
        (void)Integer(properties, "index", false, where);
        (void)Integer(properties, "repeat", false, where);
        (void)Flag(properties, "cantempty", where); (void)Flag(properties, "concat", where);
        const bool repeated = properties.contains("repeat"); // Lua中0仍为truthy，不能按数值>0判定。
        Rule rule{*id, static_cast<NativeValueType>(type), static_cast<std::uint32_t>(Integer(properties, "data", false, where)),
            repeated, Flag(properties, "profile", where), Flag(properties, "appendindex", where), Flag(properties, "splite", where)};
        if ((rule.split || rule.append) && rule.nativeType != NativeValueType::String)
            Reject("SLK_MATERIALIZER_METADATA_UNSUPPORTED", "字符串分割/append规则的类型尚未覆盖。", where);
        if ((name == "w2lobject" || rule.id == "W2lo") &&
            (name != "w2lobject" || rule.id != "W2lo" || rule.nativeType != NativeValueType::String || rule.repeated || rule.data))
            Reject("SLK_MATERIALIZER_MARKER_UNSUPPORTED", "W2lo归属metadata不符合目标库固定定义。", where);
        budget.Text(rule.id, where);
        rules.emplace(name, std::move(rule));
    }
    return rules;
}
const Rule* Lookup(const Rules& common, const Rules* specific, const std::string& key) {
    // frontend_merge 的fill先byCode，不能复用new代理common优先规则。
    if (specific) if (const auto at = specific->find(key); at != specific->end()) return &at->second;
    const auto at = common.find(key); return at == common.end() ? nullptr : &at->second;
}
SlkSessionValue CopyValue(const SlkSessionValue& value, Budget& budget, const OriginSpan& origin) {
    if (const auto* scalar = std::get_if<SlkSessionScalar>(&value)) budget.ScalarValue(*scalar, origin);
    else {
        const auto& array = std::get<std::vector<SlkSessionScalar>>(value);
        if (array.size() > kMaximumLevel) Reject("SLK_MATERIALIZER_ARRAY_UNSUPPORTED", "canonical默认数组超过256项。", origin);
        budget.Container(origin);
        for (const auto& element : array) budget.ScalarValue(element, origin);
    }
    return value;
}
SlkSessionObject CopyDefault(const std::string& id, const SlkDatasetRecord& record, Budget& budget, const OriginSpan& origin) {
    if (!Rawcode(id)) Reject("SLK_MATERIALIZER_DEFAULT_UNSUPPORTED", "canonical默认对象ID不是四字节。", origin);
    budget.Text(id, origin);
    SlkSessionObject object{id, {}};
    for (const auto& [name, value] : record.fields) {
        budget.FieldKey(name, origin);
        object.fields.emplace(name, CopyValue(value.value, budget, origin));
    }
    return object;
}
void CheckDefault(const SlkSessionObject& base, std::string_view kind, ObjectType type, const OriginSpan& origin) {
    const auto* id = String(base, "_id"); const auto* name = String(base, "_type");
    if (!id || *id != base.id || !name || *name != kind || base.fields.contains("_parent") || base.fields.contains("_obj") ||
        base.fields.contains("_create") || base.fields.contains("w2lobject"))
        Reject("SLK_MATERIALIZER_DEFAULT_UNSUPPORTED", "canonical默认身份不是未覆盖的标准对象。", origin);
    if (base.fields.contains("_code") && (!String(base, "_code") || !Rawcode(*String(base, "_code"))))
        Reject("SLK_MATERIALIZER_DEFAULT_UNSUPPORTED", "存在的_code必须是四字节字符串；不能为缺失值填造代码。", origin);
    if (base.fields.contains("_name") && !String(base, "_name"))
        Reject("SLK_MATERIALIZER_DEFAULT_UNSUPPORTED", "默认_name必须保持字符串。", origin);
    const auto levelKey = LevelField(type);
    if (!levelKey.empty()) {
        const auto* max = Scalar(base, "_max_level");
        const auto* integer = max ? std::get_if<std::int64_t>(max) : nullptr;
        if (!integer || *integer == 0 || Levels(max, origin) != Levels(Scalar(base, levelKey), origin))
            Reject("SLK_MATERIALIZER_DEFAULT_UNSUPPORTED", "默认级数缺失、不一致或为未证明的零级。", origin);
    } else if (base.fields.contains("_max_level"))
        Reject("SLK_MATERIALIZER_DEFAULT_UNSUPPORTED", "无级数类别携带未知_max_level。", origin);
}
Fields MergeFields(const SlkSessionObject& base, const Rules& common, const Rules* specific, Budget& budget, const OriginSpan& origin) {
    Fields fields;
    for (const auto& [name, value] : base.fields) if (std::holds_alternative<std::vector<SlkSessionScalar>>(value)) {
        const auto* rule = Lookup(common, specific, name);
        budget.FieldKey(name, origin); budget.Text(name, origin);
        fields.emplace(name, Field{nullptr, {name, SlkSessionFieldType::Unknown, true}, !rule || rule->profile, rule && rule->append, false});
    }
    return fields;
}
std::string TrimNativeId(const std::string& field, const OriginSpan& origin) {
    // NativeObjectCodec::IdToString 已只裁尾NUL；内部NUL仍保留，不能再次截断吞掉后续字节。
    if (field.empty() || field.size() > 4 || !std::all_of(field.begin(), field.end(), [](unsigned char c) { return c >= 0x21 && c <= 0x7e; }))
        Reject("SLK_MATERIALIZER_ADDRESS_UNSUPPORTED", "native字段ID必须是去尾NUL后的1–4个可打印字节。", origin);
    return field;
}
struct NativeEntry { const ObjectModification* source = nullptr; NativeValueType storage{}; std::size_t level = 0; };
Overlay ReadCanonicalOverlay(const NativeObjectDocument& document, const ObjectRecord& object, const SlkSessionObject& base,
    const Rules& common, const Rules* specific, OldStrings& strings, Budget& budget,
    SlkNativeFrontendMaterialization& output, const OriginSpan& origin) {
    std::map<std::string, std::vector<NativeEntry>> raw;
    std::map<std::string, std::set<std::size_t>> addresses;
    for (const auto& modification : object.modifications) {
        budget.Spend(budget.modifications, 1, 300000, origin); budget.Container(origin);
        if (modification.valueWasOpaque) Reject("SLK_MATERIALIZER_NATIVE_UNSAFE", "native覆盖含opaque数据。", origin);
        budget.Spend(budget.metadataVisits, 2 * document.Objects().size() + 2 * object.modifications.size(), 50000000, origin);
        const auto storage = document.ModificationValueType(object.key, modification.key);
        if (!storage) Reject("SLK_MATERIALIZER_NATIVE_UNSAFE", "native字段实际存储类型不唯一。", origin);
        const bool doodad = document.Type() == ObjectType::Doodad;
        if ((doodad && modification.key.level.value_or(0)) || (!doodad && modification.key.variation.value_or(0)) ||
            (LevelField(document.Type()).empty() && (modification.key.level.value_or(0) || modification.key.dataIndex.value_or(0))))
            Reject("SLK_MATERIALIZER_ADDRESS_UNSUPPORTED", "native等级/变体地址与类别不符。", origin);
        const auto rawLevel = doodad ? modification.key.variation.value_or(0) : modification.key.level.value_or(0);
        if (rawLevel > kMaximumLevel) Reject("SLK_MATERIALIZER_LEVEL_UNSUPPORTED", "native等级超出256。", origin);
        const std::size_t level = rawLevel == 0 ? 1 : rawLevel;
        const auto id = TrimNativeId(modification.key.fieldId, origin);
        if (!raw.contains(id)) { budget.FieldKey(id, origin); budget.Container(origin); }
        if (!addresses[id].insert(level).second) Reject("SLK_MATERIALIZER_DUPLICATE_ADDRESS", "native字段归一后同ID/等级重复，不能丢弃data或后写覆盖。", origin);
        raw[id].push_back({&modification, *storage, level});
    }
    Overlay edits;
    std::map<std::string, bool> repeated;
    const auto pass = [&](const Rules& rules) {
        std::set<std::string> ids;
        for (const auto& [name, rule] : rules) if (raw.contains(rule.id) && !ids.insert(rule.id).second)
            Reject("SLK_MATERIALIZER_METADATA_AMBIGUOUS", "同一metadata层有多个字段消费相同native ID，pairs顺序不可靠。", origin);
        for (const auto& [name, rule] : rules) {
            budget.Spend(budget.metadataVisits, 1, 50000000, origin);
            const auto at = raw.find(rule.id); if (at == raw.end()) continue;
            if (rule.repeated && LevelField(document.Type()).empty())
                Reject("SLK_MATERIALIZER_ADDRESS_UNSUPPORTED", "无级数类别的repeat尚未覆盖。", origin);
            budget.FieldKey(name, origin); budget.Container(origin);
            Sparse values;
            for (const auto& entry : at->second) {
                if (entry.storage != rule.nativeType || entry.source->key.dataIndex.value_or(0) != rule.data || (!rule.repeated && entry.level != 1))
                    Reject("SLK_MATERIALIZER_ADDRESS_UNSUPPORTED", "native类型/data/等级与所消费canonical规则不符。", origin);
                auto value = NativeScalar(*entry.source, entry.storage, strings, budget, output.resolvedStringReferences, origin);
                if (rule.split) { auto& text = std::get<std::string>(value); if (const auto comma = text.find(','); comma != std::string::npos) text.resize(comma); }
                values.emplace(entry.level, std::move(value)); ++output.appliedModifications;
            }
            edits.insert_or_assign(name, std::move(values)); repeated.insert_or_assign(name, rule.repeated);
            raw.erase(at); // common已消费同ID，byCode不能重复读取；不同ID则可覆盖同canonical字段。
        }
    };
    pass(common); if (specific) pass(*specific);
    if (!raw.empty()) Reject("SLK_MATERIALIZER_METADATA_UNSUPPORTED", "native字段未被common/byCode识别，不能静默丢弃。", origin);
    for (const auto& [name, isArray] : repeated) {
        const auto original = base.fields.find(name);
        if ((original == base.fields.end() && isArray) || (original != base.fields.end() &&
            std::holds_alternative<std::vector<SlkSessionScalar>>(original->second) != isArray))
            Reject("SLK_MATERIALIZER_DEFAULT_FIELD_UNKNOWN", "覆盖数组形状与默认字段不一致，或repeat缺少默认数组。", origin);
    }
    return edits;
}
void VerifyDataset(const SlkNativeFrontendInput& input, const SlkTypedDataset& dataset, const OriginSpan& origin) {
    if (dataset.coverage != SlkDatasetCoverage::DecodedPrebuiltFilesOnly || dataset.sources.size() != 11 ||
        !Hash(dataset.manifestSha256) || input.bindings.dataSetIdentity != dataset.manifestSha256 ||
        input.bindings.documentRevision != dataset.documentRevision || input.bindings.mapInfo.presence != SlkSessionSourcePresence::Present ||
        Text::LowerAscii(input.bindings.mapInfo.sha256) != Text::LowerAscii(dataset.w3iSha256) || !Hash(dataset.w3iSha256))
        Reject("SLK_MATERIALIZER_IDENTITY_MISMATCH", "canonical数据集manifest/W3I/revision与输入绑定不一致。", origin);
    std::map<std::string, const SlkDatasetSourceReceipt*> sources;
    for (const auto& source : dataset.sources) {
        if (!Hash(source.sha256) || !source.path.is_absolute() || !sources.emplace(source.role, &source).second)
            Reject("SLK_MATERIALIZER_IDENTITY_MISMATCH", "canonical来源回执缺失、重复或无绝对路径。", origin);
    }
    const auto require = [&](const std::string& role, std::string_view expected = {}) {
        const auto at = sources.find(role);
        if (at == sources.end() || (!expected.empty() && Text::LowerAscii(at->second->sha256) != Text::LowerAscii(expected)))
            Reject("SLK_MATERIALIZER_IDENTITY_MISMATCH", "canonical来源角色或SHA不匹配。", origin);
    };
    require("core-config"); require("canonical-metadata", input.bindings.metadataSha256);
    require("prebuilt-txt"); require("prebuilt-misc");
    for (std::size_t i = 0; i < kTypes.size(); ++i) {
        if (!Hash(input.bindings.defaultsSha256[i]))
            Reject("SLK_MATERIALIZER_IDENTITY_MISMATCH", "canonical默认来源摘要缺失。", origin);
        require("prebuilt-" + std::string(kNames[i]), input.bindings.defaultsSha256[i]);
    }
}
} // namespace

SlkNativeFrontendMaterialization MaterializeCanonical(const SlkNativeFrontendInput& input, Budget& budget, OldStrings& strings) {
    const auto& dataset = *input.canonicalDataset;
    const auto origin = Origin("slk-canonical-frontend", input);
    VerifyDataset(input, dataset, origin);
    std::size_t metadataCount = 0;
    if (dataset.metadata.byCode.size() > 10000)
        Reject("SLK_MATERIALIZER_LIMIT", "canonical metadata scope超过10000。", origin);
    const auto count = [&](const SlkDatasetMetadataScope& scope) { budget.Spend(metadataCount, scope.fields.size(), 10000, origin); };
    for (const auto& scope : dataset.metadata.common) count(scope);
    count(dataset.metadata.misc);
    for (const auto& [code, scope] : dataset.metadata.byCode) { (void)code; count(scope); }
    std::array<Rules, 7> common;
    for (std::size_t i = 0; i < common.size(); ++i) common[i] = ReadRules(dataset, dataset.metadata.common[i], budget, origin);
    std::map<std::string, Rules> specific;
    for (const auto& [code, scope] : dataset.metadata.byCode) {
        budget.FieldKey(code, origin); specific.emplace(code, ReadRules(dataset, scope, budget, origin));
    }
    SlkNativeFrontendMaterialization output;
    output.canonicalDatasetManifest = dataset.manifestSha256;
    for (std::size_t i = 0; i < kTypes.size(); ++i) {
        VerifyNativeIdentity(input, i, budget);
        budget.Spend(budget.objects, dataset.defaults[i].size(), 20000, origin);
        auto& result = output.session.classes[i]; result.type = kTypes[i];
        std::map<std::string, const ObjectRecord*> records;
        if (input.nativeDocuments[i]) for (const auto& record : input.nativeDocuments[i]->Objects()) {
            budget.FieldKey(record.key.rawcode, origin);
            if (record.key.type != kTypes[i] || !Rawcode(record.key.rawcode) || !records.emplace(record.key.rawcode, &record).second)
                Reject("SLK_MATERIALIZER_NATIVE_UNSAFE", "native对象ID无效或重复。", origin);
            const auto parent = record.parentRawcode.value_or(record.key.rawcode);
            if (!dataset.defaults[i].contains(parent) || (record.parentRawcode && dataset.defaults[i].contains(record.key.rawcode)))
                Reject("SLK_MATERIALIZER_PARENT_UNSUPPORTED", "native父对象不在标准默认，或custom覆盖标准ID。", origin);
        }
        std::map<std::string, std::vector<const ObjectRecord*>> children;
        for (const auto& [id, record] : records) {
            (void)id;
            const auto& parent = record->parentRawcode ? *record->parentRawcode : record->key.rawcode;
            if (!children.contains(parent)) budget.FieldKey(parent, origin);
            budget.Container(origin);
            children[parent].push_back(record);
        }
        for (const auto& [id, source] : dataset.defaults[i]) {
            const auto where = Where(dataset, source.source, origin);
            auto base = CopyDefault(id, source, budget, where);
            CheckDefault(base, kNames[i], kTypes[i], where);
            const Rules* byCode = nullptr;
            if (const auto* code = String(base, "_code")) { const auto at = specific.find(*code); if (at != specific.end()) byCode = &at->second; }
            const auto fields = MergeFields(base, common[i], byCode, budget, where);
            if (!records.contains(id)) result.existingObjects.push_back(Merge(base, fields, {}, kTypes[i], budget, where));
            if (const auto at = children.find(id); at != children.end()) for (const auto* record : at->second) {
                const auto objectOrigin = Origin(NativeFileName(kTypes[i]), input);
                const auto edits = ReadCanonicalOverlay(*input.nativeDocuments[i], *record, base, common[i], byCode, strings, budget, output, objectOrigin);
                auto object = Merge(base, fields, edits, kTypes[i], budget, objectOrigin);
                object.id = record->key.rawcode;
                budget.ScalarValue(object.id, objectOrigin); object.fields["_id"] = Value(object.id);
                budget.FieldKey("_parent", objectOrigin); budget.ScalarValue(id, objectOrigin); object.fields["_parent"] = Value(id);
                budget.FieldKey("_obj", objectOrigin); budget.ScalarValue(true, objectOrigin); object.fields["_obj"] = Value(true);
                if (String(object, "w2lobject")) output.generatedObjectIds[i].push_back(object.id);
                result.existingObjects.push_back(std::move(object));
            }
            result.standardDefaults.push_back(std::move(base));
        }
        // 没有把byCode压成类别单表；Session原有metadata布局不足以授权这份canonical图。
    }
    return output;
}
} // namespace War3Map::Script::MaterializerDetail
