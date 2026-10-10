#include "SlkMaterializerInternal.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

// 独立实现，没有复制 Lua 源码。已读本机 YDWE 的 frontend_obj.lua 22–81、
// frontend_updateobj.lua 29–82、frontend_merge.lua 20–157 与 core/init.lua 149–171。
// frontend_obj SHA256: 350DD4BDF8A1AB713F87A45CB62327FC96A2D4A35B803BDC2476A8528EEF82DD
// frontend_updateobj: CED40B13E252613599F67A08A771D28B769917D7DC6DEC83C5710681AC3DCE2A
// frontend_merge: 3145DB66B0AF41859A39D8574A93B4BF31DBB4269D8654833F8098F620FA481F
// 其余来源身份见 docs/SLK_TEMPLATE_SEMANTICS_2026-09-26.md。
namespace War3Map::Script {
namespace MaterializerDetail {
[[noreturn]] void Reject(std::string code, std::string message, const OriginSpan& origin) {
    throw Rejection{std::move(code), std::move(message), origin};
}
OriginSpan Origin(std::string_view path, const SlkNativeFrontendInput& input) {
    return {std::string(path), 0, 0, input.bindings.candidateSha256};
}
bool Hash(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}
bool Rawcode(std::string_view value) {
    return value.size() == 4 && std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 0x21 && c <= 0x7e; });
}
const SlkSessionScalar* Scalar(const SlkSessionObject& object, std::string_view field) {
    const auto found = object.fields.find(std::string(field));
    return found == object.fields.end() ? nullptr : std::get_if<SlkSessionScalar>(&found->second);
}
const std::string* String(const SlkSessionObject& object, std::string_view field) {
    const auto* scalar = Scalar(object, field);
    return scalar ? std::get_if<std::string>(scalar) : nullptr;
}
SlkSessionValue Value(SlkSessionScalar value) { return value; }
std::string_view LevelField(ObjectType type) {
    if (type == ObjectType::Ability) return "levels";
    if (type == ObjectType::Upgrade) return "maxlevel";
    if (type == ObjectType::Doodad) return "numvar";
    return {};
}
std::size_t Levels(const SlkSessionScalar* value, const OriginSpan& origin) {
    const auto* integer = value ? std::get_if<std::int64_t>(value) : nullptr;
    if (!integer || *integer < 0 || *integer > static_cast<std::int64_t>(kMaximumLevel))
        Reject("SLK_MATERIALIZER_LEVEL_UNSUPPORTED", "等级必须是 0–256 范围的整数类型。", origin);
    return *integer == 0 ? 1 : static_cast<std::size_t>(*integer);
}
SlkSessionFieldType LuaType(NativeValueType value) {
    return value == NativeValueType::String ? SlkSessionFieldType::String : SlkSessionFieldType::Number;
}
void CheckType(const SlkSessionScalar& value, SlkSessionFieldType expected, const OriginSpan& origin) {
    if (expected == SlkSessionFieldType::Unknown) return; // canonical 原始默认数组只复制 typed 值，不做字段类型推断。
    const bool number = std::holds_alternative<std::int64_t>(value) || std::holds_alternative<double>(value);
    if ((expected == SlkSessionFieldType::Boolean && std::holds_alternative<bool>(value)) ||
        (expected == SlkSessionFieldType::Number && number) ||
        (expected == SlkSessionFieldType::String && std::holds_alternative<std::string>(value))) return;
    Reject("SLK_MATERIALIZER_VALUE_TYPE", "值与已确认 metadata 的 Lua 类型不一致；bool 不转换为整数。", origin);
}
Fields ResolveFields(const SlkNativeFrontendInput& input, ObjectType type, const SlkSessionObject& base,
    Budget& budget, const OriginSpan& origin) {
    ObjectFieldContext context{base.id, *String(base, "_code")};
    std::set<std::string> indexed;
    budget.Spend(budget.metadataVisits, input.schema->Definitions().size(), 50000000, origin);
    for (const auto& definition : input.schema->Definitions()) {
        if (definition.objectType == type && definition.index >= 1) indexed.insert(definition.sourceField);
    }
    Fields fields;
    for (const auto& definition : input.schema->Definitions()) {
        budget.Spend(budget.metadataVisits, 1, 50000000, origin);
        if (definition.objectType != type) continue;
        budget.Spend(budget.metadataVisits, definition.useSpecific.size(), 50000000, origin);
        budget.Spend(budget.metadataVisits, definition.notSpecific.size(), 50000000, origin);
        if (const auto section = definition.metadata.find("section"); section != definition.metadata.end() && !section->second.empty())
            Reject("SLK_MATERIALIZER_METADATA_UNSUPPORTED", "metadata section/codemapped 适用范围尚未覆盖。", origin);
        if (!definition.notSpecific.empty())
            Reject("SLK_MATERIALIZER_METADATA_UNSUPPORTED", "metadata notSpecific 与目标 frontend 的适用规则尚未对照。", origin);
        if (!input.schema->AppliesTo(definition, context)) continue;
        if (definition.fieldId == "W2lo") continue; // YDWE add_user_metadata 固定覆盖此内部定义
        if (!definition.nativeValueType || definition.repeat < 0 || definition.data < 0 || definition.data > 9 ||
            definition.appendIndex < 0 || definition.appendIndex > 1 || !definition.effectType.empty() || definition.effectIndex != 0)
            Reject("SLK_MATERIALIZER_METADATA_UNSUPPORTED", "metadata 类型、data 或 effect 映射尚未覆盖。", origin);
        std::string key = definition.sourceField;
        if (definition.data) key += static_cast<char>('A' + definition.data - 1);
        if (indexed.contains(definition.sourceField)) {
            if (definition.index < 0 || definition.index > 255)
                Reject("SLK_MATERIALIZER_METADATA_UNSUPPORTED", "成组 profile 字段缺少可靠分量索引。", origin);
            key += '_' + std::to_string(definition.index + 1);
        }
        key = Text::LowerAscii(key);
        if (key.empty() || key.size() > 256 || key.front() == '_')
            Reject("SLK_MATERIALIZER_METADATA_UNSUPPORTED", "规范字段名无效。", origin);
        const bool profile = Text::LowerAscii(definition.sourceTable) == "profile";
        const auto kind = LuaType(*definition.nativeValueType);
        const bool repeated = !LevelField(type).empty() && definition.repeat > 0;
        if (definition.repeat > 0 && !repeated)
            Reject("SLK_MATERIALIZER_METADATA_UNSUPPORTED", "无等级类别中的 repeat 尚未覆盖。", origin);
        if (definition.appendIndex && (!profile || kind != SlkSessionFieldType::String))
            Reject("SLK_MATERIALIZER_METADATA_UNSUPPORTED", "appendIndex 只支持已确认的 profile 字符串字段。", origin);
        const auto sourceType = Text::LowerAscii(definition.valueType);
        bool split = false;
        if (*definition.nativeValueType == NativeValueType::String && definition.index == -1) {
            if (definition.isList || sourceType == "defensetable") split = false;
            else if (sourceType == "string" || sourceType == "char" || sourceType == "model" || sourceType == "icon" ||
                sourceType == "soundlabel" || sourceType == "abilcode" || sourceType == "unitcode" || sourceType == "upgradecode") split = true;
            else Reject("SLK_MATERIALIZER_METADATA_UNSUPPORTED", "字符串字段的 splite/concat 分类尚未覆盖。", origin);
        }
        budget.FieldKey(key, origin); // map 条目和保留的 canonical metadata 名称都须预先记账。
        budget.Text(key, origin);
        Field field{&definition, {key, kind, repeated}, profile, definition.appendIndex == 1, split};
        if (!fields.emplace(key, std::move(field)).second)
            Reject("SLK_MATERIALIZER_METADATA_AMBIGUOUS", "同 baseCode 的规范字段匹配多个 native ID。", origin);
    }
    return fields;
}

void CheckBase(const SlkSessionObject& base, std::string_view type, const OriginSpan& origin) {
    const auto* id = String(base, "_id");
    const auto* kind = String(base, "_type");
    const auto* code = String(base, "_code");
    if (!Rawcode(base.id) || !id || *id != base.id || !kind || *kind != type || !code || !Rawcode(*code) ||
        base.fields.contains("_parent") || base.fields.contains("w2lobject") || base.fields.contains("_create"))
        Reject("SLK_MATERIALIZER_DEFAULT_UNSUPPORTED", "需要标准 typed default，不能用覆盖后父对象或显示目录代替。", origin);
}
void CheckLevelDefault(const SlkSessionObject& base, ObjectType type, const OriginSpan& origin) {
    const auto key = LevelField(type);
    if (key.empty()) {
        if (base.fields.contains("_max_level"))
            Reject("SLK_MATERIALIZER_DEFAULT_UNSUPPORTED", "无等级类别的 default 不能携带 _max_level。", origin);
        return;
    }
    if (Levels(Scalar(base, key), origin) != Levels(Scalar(base, "_max_level"), origin))
        Reject("SLK_MATERIALIZER_DEFAULT_UNSUPPORTED", "default 级数字段与 _max_level 不一致。", origin);
}
void CheckDefaultFields(const SlkSessionObject& base, const Fields& fields, Budget& budget, const OriginSpan& origin) {
    for (const auto& [key, value] : base.fields) {
        budget.FieldKey(key, origin); // 对应输出中保留的原始 standardDefaults 副本。
        if (key == "_id" || key == "_code" || key == "_type" || key == "_max_level") {
            const auto* scalar = std::get_if<SlkSessionScalar>(&value);
            if (!scalar) Reject("SLK_MATERIALIZER_DEFAULT_UNSUPPORTED", "内部默认字段必须是标量。", origin);
            budget.ScalarValue(*scalar, origin);
            continue;
        }
        const auto definition = fields.find(key);
        if (definition == fields.end()) Reject("SLK_MATERIALIZER_DEFAULT_FIELD_UNKNOWN", "default 字段无法唯一匹配 metadata：" + key, origin);
        const auto& field = definition->second;
        if (field.session.array) {
            const auto* values = std::get_if<std::vector<SlkSessionScalar>>(&value);
            if (!values || values->size() > kMaximumLevel)
                Reject("SLK_MATERIALIZER_ARRAY_UNSUPPORTED", "default 重复字段不是有界稠密数组。", origin);
            budget.Container(origin);
            for (const auto& item : *values) { CheckType(item, field.session.type, origin); budget.ScalarValue(item, origin); }
        } else {
            const auto* scalar = std::get_if<SlkSessionScalar>(&value);
            if (!scalar) Reject("SLK_MATERIALIZER_ARRAY_UNSUPPORTED", "非重复 default 字段不能是数组。", origin);
            CheckType(*scalar, field.session.type, origin); budget.ScalarValue(*scalar, origin);
        }
    }
}
std::vector<SlkSessionScalar> Fill(const std::vector<SlkSessionScalar>& base, const Sparse* edits,
    std::size_t levels, const Field& field, Budget& budget, const OriginSpan& origin) {
    std::vector<SlkSessionScalar> result;
    budget.Container(origin); // 空表也分配容器，不能靠零元素绕过预算。
    if (base.empty() && (!edits || edits->empty())) return result; // nil 填充仍是空表
    const auto lastEdited = edits && !edits->empty() ? edits->rbegin()->first : 0;
    for (std::size_t level = 1; level <= levels; ++level) {
        std::optional<SlkSessionScalar> selected;
        if (edits) { const auto found = edits->find(level); if (found != edits->end()) selected = found->second; }
        if (!selected) {
            if (level <= base.size()) selected = base[level - 1];
            else if (!edits) { if (!base.empty()) selected = base.back(); }
            else if (field.append) selected = std::string{};
            else if (field.profile) {
                if (level <= lastEdited) selected = std::string{};
                else if (!result.empty()) selected = result.back();
            } else if (!base.empty()) selected = base.back();
        }
        if (!selected) Reject("SLK_MATERIALIZER_ARRAY_UNSUPPORTED", "fill 产生稀疏表，当前稠密数组子集不能表示。", origin);
        CheckType(*selected, field.session.type, origin);
        budget.ScalarValue(*selected, origin);
        result.push_back(std::move(*selected));
    }
    return result;
}

OldStrings::OldStrings(const SlkNativeFrontendInput& input, Budget& budget) : budget_(budget) {
        const auto origin = Origin("war3map.wts", input);
        if (input.bindings.strings.presence == SlkSessionSourcePresence::Missing) {
            if (!input.bindings.strings.sha256.empty() || input.strings)
                Reject("SLK_MATERIALIZER_IDENTITY_MISMATCH", "WTS 被标为不存在但提供了字符串或摘要。", origin);
            return;
        }
        if (input.bindings.strings.presence != SlkSessionSourcePresence::Present || !input.strings)
            Reject("SLK_MATERIALIZER_IDENTITY_MISMATCH", "WTS 的存在性未验证。", origin);
        if (input.strings->entries.size() > input.limits.wtsEntries || input.strings->originalBytes.size() > 64 * 1024 * 1024)
            Reject("SLK_MATERIALIZER_LIMIT", "WTS 条目或原字节超过索引前预算。", origin);
        budget_.Spend(budget_.values, input.strings->entries.size(), input.limits.values, origin);
        // 必须先完成所有值的累计预检，再建立索引或调用 Serialize。
        for (const auto& entry : input.strings->entries) budget_.Text(entry.value, origin);
        for (const auto& entry : input.strings->entries) {
            if (!entries_.emplace(entry.id, &entry.value).second) Reject("SLK_MATERIALIZER_WTS_AMBIGUOUS", "WTS ID 重复。", origin);
        }
        const auto bytes = input.strings->Serialize();
        if (!bytes.Succeeded()) Reject("SLK_MATERIALIZER_WTS_INVALID", "WTS 无法可靠序列化。", origin);
        const auto hash = Sha256(*bytes.value);
        if (!hash.Succeeded() || Text::LowerAscii(*hash.value) != Text::LowerAscii(input.bindings.strings.sha256))
            Reject("SLK_MATERIALIZER_IDENTITY_MISMATCH", "WTS 最终字节与绑定身份不符。", origin);
    }
std::string OldStrings::Resolve(std::string_view value, const OriginSpan& origin, std::size_t& references) {
        std::string result;
        const auto append = [&](std::string_view text) {
            if (text.size() > 1024 * 1024 - result.size())
                Reject("SLK_MATERIALIZER_LIMIT", "WTS 展开超过单字符串预算。", origin);
            result += text;
        };
        std::size_t at = 0;
        while (at < value.size()) {
            const auto marker = value.find("TRIGSTR_", at);
            if (marker == std::string_view::npos) { append(value.substr(at)); break; }
            append(value.substr(at, marker - at));
            std::size_t end = marker + 8;
            while (end < value.size() && value[end] >= '0' && value[end] <= '9') ++end;
            if (end == marker + 8) { append("TRIGSTR_"); at = end; continue; }
            std::uint32_t id = 0;
            const auto parsed = std::from_chars(value.data() + marker + 8, value.data() + end, id);
            const auto found = entries_.find(id);
            if (parsed.ec != std::errc{} || found == entries_.end())
                Reject("SLK_MATERIALIZER_WTS_MISSING", "old 字符串包含缺失或超出范围的 WTS 引用。", origin);
            append(*found->second); ++references; at = end;
        }
        budget_.Text(result, origin);
        return result; // 不再次扫描替换文本，和 gsub 的单次替换一致
    }
SlkSessionScalar NativeScalar(const ObjectModification& modification, NativeValueType type,
    OldStrings& strings, Budget& budget, std::size_t& references, const OriginSpan& origin) {
    SlkSessionScalar result;
    if (type == NativeValueType::Int32) {
        const auto* value = std::get_if<std::int32_t>(&modification.value);
        if (!value) Reject("SLK_MATERIALIZER_VALUE_TYPE", "native Int32 的值类型不一致。", origin);
        result = static_cast<std::int64_t>(*value);
    } else if (type == NativeValueType::Real || type == NativeValueType::Unreal) {
        const auto* value = std::get_if<float>(&modification.value);
        if (!value || !std::isfinite(*value)) Reject("SLK_MATERIALIZER_VALUE_TYPE", "native Real/Unreal 缺少有限 Float32。", origin);
        result = static_cast<double>(*value);
    } else {
        const auto* value = std::get_if<std::string>(&modification.value);
        if (!value) Reject("SLK_MATERIALIZER_VALUE_TYPE", "native String 的值类型不一致。", origin);
        budget.Text(*value, origin); result = strings.Resolve(*value, origin, references);
    }
    budget.ScalarValue(result, origin);
    return result;
}

Overlay ReadOverlay(const NativeObjectDocument& document, const ObjectRecord& object, const Fields& fields, const ObjectFieldSchema& schema,
    const SlkSessionObject& base, OldStrings& strings, Budget& budget, SlkNativeFrontendMaterialization& output,
    const OriginSpan& objectOrigin) {
    Overlay result;
    ObjectFieldContext context{base.id, *String(base, "_code")};
    for (const auto& modification : object.modifications) {
        budget.Spend(budget.modifications, 1, 300000, objectOrigin);
        auto origin = modification.origin.value_or(objectOrigin);
        origin.source = objectOrigin.source; origin.fingerprint = objectOrigin.fingerprint;
        if (modification.valueWasOpaque) Reject("SLK_MATERIALIZER_NATIVE_UNSAFE", "native 覆盖含 opaque 数据。", origin);
        // 现有真实存储类型接口会线性寻找对象/字段；为此也记入有界匹配预算。
        budget.Spend(budget.metadataVisits, 2 * document.Objects().size(), 50000000, origin);
        budget.Spend(budget.metadataVisits, 2 * object.modifications.size(), 50000000, origin);
        const auto storage = document.ModificationValueType(object.key, modification.key);
        if (!storage) Reject("SLK_MATERIALIZER_NATIVE_UNSAFE", "native 存储类型或字段地址不唯一。", origin);
        if ((document.Type() == ObjectType::Doodad && modification.key.level.value_or(0)) ||
            (document.Type() != ObjectType::Doodad && modification.key.variation.value_or(0)))
            Reject("SLK_MATERIALIZER_ADDRESS_UNSUPPORTED", "native 地址混用了等级与变体。", origin);
        if (LevelField(document.Type()).empty() && (modification.key.level.value_or(0) || modification.key.dataIndex.value_or(0)))
            Reject("SLK_MATERIALIZER_ADDRESS_UNSUPPORTED", "无扩展地址的 native 类别不能携带等级/data。", origin);
        const auto rawLevel = document.Type() == ObjectType::Doodad ? modification.key.variation.value_or(0) : modification.key.level.value_or(0);
        if (rawLevel > kMaximumLevel) Reject("SLK_MATERIALIZER_LEVEL_UNSUPPORTED", "native 等级超过子集上限。", origin);
        const std::size_t level = rawLevel == 0 ? 1 : rawLevel;
        std::string canonical;
        bool split = false;
        if (modification.key.fieldId == "W2lo") {
            if (*storage != NativeValueType::String || level != 1 || modification.key.dataIndex.value_or(0))
                Reject("SLK_MATERIALIZER_MARKER_UNSUPPORTED", "W2lo 必须是唯一第一级/无 data 索引的原生字符串。", origin);
            canonical = "w2lobject";
        } else {
            const Field* field = nullptr;
            budget.Spend(budget.metadataVisits, fields.size(), 50000000, origin);
            for (const auto& [key, candidate] : fields) {
                if (candidate.definition->fieldId == modification.key.fieldId) {
                    if (field) Reject("SLK_MATERIALIZER_METADATA_AMBIGUOUS", "同 native ID 有多个适用字段。", origin);
                    field = &candidate; canonical = key;
                }
            }
            if (!field) Reject("SLK_MATERIALIZER_METADATA_UNSUPPORTED", "native 字段没有唯一适用的 metadata。", origin);
            auto normalized = modification.key;
            if (document.Type() == ObjectType::Doodad && field->session.array)
                Reject("SLK_MATERIALIZER_VARIATION_UNPROVEN", "doodad 变体覆盖编号尚无独立对照。", origin);
            if (document.Type() == ObjectType::Ability || document.Type() == ObjectType::Upgrade)
                normalized.level = field->session.array ? static_cast<std::uint32_t>(level) : 0;
            if (document.Type() == ObjectType::Doodad) normalized.variation = 0;
            if (!field->session.array && level != 1)
                Reject("SLK_MATERIALIZER_ADDRESS_UNSUPPORTED", "非重复字段的额外等级不能静默丢弃。", origin);
            const auto match = schema.MatchNativeField(document.Type(), normalized, context);
            if (!match.safeToEdit || match.definition != field->definition || *storage != *field->definition->nativeValueType ||
                modification.key.dataIndex.value_or(0) != static_cast<std::uint32_t>(field->definition->data))
                Reject("SLK_MATERIALIZER_ADDRESS_UNSUPPORTED", "native 存储类型或 data 索引与 metadata 不符。", origin);
            split = field->split;
        }
        auto value = NativeScalar(modification, *storage, strings, budget, output.resolvedStringReferences, origin);
        if (split) {
            auto& text = std::get<std::string>(value);
            if (const auto comma = text.find(','); comma != std::string::npos) text.resize(comma);
        }
        if (!result[canonical].emplace(level, std::move(value)).second)
            Reject("SLK_MATERIALIZER_DUPLICATE_ADDRESS", "多个 native 覆盖归一到同字段/等级；不能按最后值吞掉。", origin);
        ++output.appliedModifications;
    }
    return result;
}

SlkSessionObject Merge(const SlkSessionObject& base, const Fields& fields, const Overlay& edits,
    ObjectType type, Budget& budget, const OriginSpan& origin) {
    const auto levelKey = LevelField(type);
    std::size_t levels = 0;
    if (!levelKey.empty()) {
        const auto max = edits.find(std::string(levelKey));
        levels = Levels(max == edits.end() ? Scalar(base, "_max_level") : &max->second.begin()->second, origin);
    }
    SlkSessionObject result;
    result.id = base.id;
    std::set<std::string> consumed;
    for (const auto& [key, value] : base.fields) {
        budget.FieldKey(key, origin);
        const auto overlay = edits.find(key);
        if (overlay != edits.end()) consumed.insert(key);
        if (const auto* array = std::get_if<std::vector<SlkSessionScalar>>(&value)) {
            const auto field = fields.find(key);
            if (levels == 0 || field == fields.end()) Reject("SLK_MATERIALIZER_ARRAY_UNSUPPORTED", "数组缺少可靠等级或 metadata。", origin);
            result.fields[key] = Fill(*array, overlay == edits.end() ? nullptr : &overlay->second, levels, field->second, budget, origin);
        } else {
            const auto& scalar = overlay == edits.end() ? std::get<SlkSessionScalar>(value) : overlay->second.begin()->second;
            budget.ScalarValue(scalar, origin);
            result.fields[key] = scalar;
        }
    }
    for (const auto& [key, values] : edits) {
        if (consumed.contains(key)) continue;
        if (const auto field = fields.find(key); field != fields.end() && field->second.session.array)
            Reject("SLK_MATERIALIZER_DEFAULT_FIELD_UNKNOWN", "重复覆盖没有标准 default 数组，不能猜测空表填充。", origin);
        budget.FieldKey(key, origin);
        budget.ScalarValue(values.begin()->second, origin);
        result.fields[key] = values.begin()->second;
    }
    if (!levelKey.empty()) result.fields["_max_level"] = Value(static_cast<std::int64_t>(levels));
    return result;
}

void VerifyNativeIdentity(const SlkNativeFrontendInput& input, std::size_t index, Budget& budget) {
    const auto origin = Origin(ObjectData::NativeFileName(kTypes[index]), input);
    const auto* document = input.nativeDocuments[index];
    const auto& identity = input.bindings.nativeMembers[index];
    if (identity.presence == SlkSessionSourcePresence::Missing) {
        if (document || !identity.sha256.empty()) Reject("SLK_MATERIALIZER_IDENTITY_MISMATCH", "缺失 native 成员不能提供文档或摘要。", origin);
        return;
    }
    if (identity.presence != SlkSessionSourcePresence::Present || !Hash(identity.sha256) || !document ||
        document->Type() != kTypes[index] || document->Version() != 2 || !document->IsStructurallySafe() || !document->IsEditable())
        Reject("SLK_MATERIALIZER_NATIVE_UNSAFE", "需要身份明确、结构可编辑的经典 native v2 文档。", origin);
    budget.Spend(budget.objects, document->Objects().size(), 20000, origin);
    std::size_t modifications = 0;
    for (const auto& object : document->Objects()) {
        if (object.modifications.size() > 300000 - modifications) Reject("SLK_MATERIALIZER_LIMIT", "native 覆盖数量过大。", origin);
        modifications += object.modifications.size();
        for (const auto& field : object.modifications) {
            if (const auto* text = std::get_if<std::string>(&field.value)) budget.Text(*text, origin);
        }
    }
    const NativeObjectCodec codec(kTypes[index]);
    const auto bytes = codec.Serialize(*document);
    if (!bytes.Succeeded()) Reject("SLK_MATERIALIZER_NATIVE_UNSAFE", "native 文档无法可靠序列化。", origin);
    const auto digest = Sha256(*bytes.value);
    if (!digest.Succeeded() || Text::LowerAscii(*digest.value) != Text::LowerAscii(identity.sha256))
        Reject("SLK_MATERIALIZER_IDENTITY_MISMATCH", "native 最终字节与绑定摘要不同。", origin);
}
} // namespace MaterializerDetail
using namespace MaterializerDetail;

Result<SlkNativeFrontendMaterialization> MaterializeSlkNativeFrontend(const SlkNativeFrontendInput& input) {
    try {
        const auto origin = Origin("slk-native-frontend", input);
        const bool canonical = input.canonicalDataset != nullptr;
        const bool mixed = canonical && (input.schema || std::any_of(input.standardDefaults.begin(), input.standardDefaults.end(),
            [](const auto& objects) { return !objects.empty(); }));
        if (mixed || (!canonical && (!input.schema || input.schema->Definitions().empty())) || !input.verifiedNativeOnlySources ||
            !input.sourceValidationDiagnostics.empty())
            Reject("SLK_MATERIALIZER_SOURCES_UNPROVEN", "缺少可靠 metadata 或原生成员来源闭合证明。", origin);
        if (!Hash(input.bindings.candidateSha256) || !Hash(input.bindings.metadataSha256) || input.bindings.dataSetIdentity.empty())
            Reject("SLK_MATERIALIZER_IDENTITY_MISMATCH", "候选、metadata 或数据集身份缺失。", origin);
        const SlkNativeFrontendLimits hardLimits;
        if (input.limits.values > hardLimits.values || input.limits.stringBytes > hardLimits.stringBytes ||
            input.limits.wtsEntries > hardLimits.wtsEntries)
            Reject("SLK_MATERIALIZER_LIMIT", "调用方只能收紧物化硬预算。", origin);
        Budget budget{input.limits};
        if (!canonical && input.schema->Definitions().size() > 10000)
            Reject("SLK_MATERIALIZER_LIMIT", "metadata 定义超过 10000 条。", origin);
        OldStrings strings(input, budget);
        SlkNativeFrontendMaterialization output = canonical ? MaterializeCanonical(input, budget, strings) : SlkNativeFrontendMaterialization{};
        output.session.bindings = input.bindings;
        for (std::size_t i = 0; !canonical && i < kTypes.size(); ++i) {
            const auto classOrigin = Origin("slk-default/" + std::string(kNames[i]), input);
            if (!Hash(input.bindings.defaultsSha256[i])) Reject("SLK_MATERIALIZER_IDENTITY_MISMATCH", "typed default 来源摘要缺失。", classOrigin);
            VerifyNativeIdentity(input, i, budget);
            auto& result = output.session.classes[i];
            result.type = kTypes[i];
            budget.Spend(budget.objects, input.standardDefaults[i].size(), 20000, classOrigin);
            std::map<std::string, const SlkSessionObject*> bases;
            std::map<std::string, Fields> resolved;
            std::map<std::string, SlkSessionFieldDefinition> classFields;
            std::map<std::string, SlkSessionObject> objects;
            const bool hasSpecific = std::any_of(input.schema->Definitions().begin(), input.schema->Definitions().end(),
                [&](const auto& field) { return field.objectType == kTypes[i] && !field.useSpecific.empty(); });
            for (const auto& base : input.standardDefaults[i]) {
                CheckBase(base, kNames[i], classOrigin);
                CheckLevelDefault(base, kTypes[i], classOrigin);
                if (hasSpecific && *String(base, "_code") != base.id)
                    Reject("SLK_MATERIALIZER_METADATA_UNSUPPORTED", "此类别同时包含别名 baseCode 与 useSpecific，需先物化完整 codemapped。", classOrigin);
                if (!bases.emplace(base.id, &base).second) Reject("SLK_MATERIALIZER_DEFAULT_UNSUPPORTED", "标准 default ID 重复。", classOrigin);
                auto fields = ResolveFields(input, kTypes[i], base, budget, classOrigin);
                CheckDefaultFields(base, fields, budget, classOrigin);
                for (const auto& [key, field] : fields) {
                    const auto at = classFields.find(key);
                    if (at == classFields.end()) {
                        budget.FieldKey(key, classOrigin);
                        budget.Text(field.session.name, classOrigin);
                        classFields.emplace(key, field.session);
                    } else if (at->second.type != field.session.type || at->second.array != field.session.array)
                        Reject("SLK_MATERIALIZER_BASE_SCHEMA_UNSUPPORTED", "不同 baseCode 的同名字段类型不同，当前 Session metadata 不能表示。", classOrigin);
                }
                objects.emplace(base.id, Merge(base, fields, {}, kTypes[i], budget, classOrigin));
                resolved.emplace(base.id, std::move(fields));
            }
            if (const auto* document = input.nativeDocuments[i]) {
                std::set<std::string> seen;
                for (const auto& record : document->Objects()) {
                    const auto objectOrigin = Origin(ObjectData::NativeFileName(kTypes[i]), input);
                    if (record.key.type != kTypes[i] || !Rawcode(record.key.rawcode) || !seen.insert(record.key.rawcode).second)
                        Reject("SLK_MATERIALIZER_NATIVE_UNSAFE", "native 对象身份无效或重复。", objectOrigin);
                    const auto parent = record.parentRawcode.value_or(record.key.rawcode);
                    const auto base = bases.find(parent);
                    if (base == bases.end() || (record.parentRawcode && bases.contains(record.key.rawcode)))
                        Reject("SLK_MATERIALIZER_PARENT_UNSUPPORTED", "原版覆盖或 custom 父对象不在标准 default，或 custom ID 与原版冲突。", objectOrigin);
                    const auto& fields = resolved.at(parent);
                    const auto edits = ReadOverlay(*document, record, fields, *input.schema, *base->second, strings, budget, output, objectOrigin);
                    auto object = Merge(*base->second, fields, edits, kTypes[i], budget, objectOrigin);
                    object.id = record.key.rawcode;
                    budget.ScalarValue(object.id, objectOrigin);
                    object.fields["_id"] = Value(object.id);
                    budget.FieldKey("_parent", objectOrigin); budget.ScalarValue(parent, objectOrigin);
                    object.fields["_parent"] = Value(parent);
                    budget.FieldKey("_obj", objectOrigin); budget.ScalarValue(true, objectOrigin);
                    object.fields["_obj"] = Value(true);
                    objects.insert_or_assign(object.id, std::move(object));
                }
            }
            result.standardDefaults = input.standardDefaults[i];
            for (auto& [id, object] : objects) {
                if (const auto* marker = String(object, "w2lobject"); marker) output.generatedObjectIds[i].push_back(id);
                result.existingObjects.push_back(std::move(object));
            }
            for (auto& [key, field] : classFields) { (void)key; result.metadata.push_back(std::move(field)); }
            // 不设置 Complete：这里只构造 old，没有顺序 new/fill 与真实工具 oracle。
        }
        output.session.unresolvedSemantics = {"ExistingNativeFrontendOnly：尚未物化全部模板的有序 new 对象。",
            canonical ? "canonical 数据集回执已交叉绑定；公开 typed 图的来源复验、完整 map frontend 与目标 DLL oracle 尚未闭合。"
                : "typed default/raw metadata 的来源对应关系与真实 YDWE frontend oracle 仍须由后续适配器验证。"};
        if (canonical) output.session.unresolvedSemantics.push_back("canonical common/byCode 未压成 Session 的类别单表；per-baseCode 完整证明适配尚未完成。");
        Diagnostic pending{DiagnosticSeverity::Warning, "Script.SlkMaterializer", "SLK_MATERIALIZER_SESSION_UNVERIFIED",
            "已有对象子集已构造；Session 仍为 Unverified，不能据此跳过模板或保存。", origin};
        output.session.materializationDiagnostics.push_back(pending);
        auto result = Result<SlkNativeFrontendMaterialization>::Success(std::move(output));
        result.diagnostics.push_back(std::move(pending));
        return result;
    } catch (const Rejection& failure) {
        auto result = Result<SlkNativeFrontendMaterialization>::Failure("Script.SlkMaterializer", failure.code, failure.message);
        result.diagnostics.front().origin = failure.origin;
        return result;
    } catch (const std::exception&) {
        return Result<SlkNativeFrontendMaterialization>::Failure("Script.SlkMaterializer", "SLK_MATERIALIZER_RESOURCE_FAILURE",
            "已有对象物化未完成，未返回部分图。");
    }
}
} // namespace War3Map::Script
