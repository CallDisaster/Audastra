#include "War3MapCore/Script/SlkTemplateSession.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

// 独立实现，未复制 Lua 实现。已审计目标 slk_lib.lua 的 SHA 见 kLibrarySha256：
// mark_obj 534–555；eq_obj 702–722；update_change 724–738；refresh 742–768。
// 仅证明 refresh 的 need_update 全空，绝不假定 backend_cleanobj 可以抵消一次更新。
namespace War3Map::Script {
namespace {
using ObjectType = ObjectData::ObjectType;
constexpr std::string_view kLibrarySha256 = "9ba2c9b8e5b53005cf5714e5188e533186d97f770c398e47e998b2b3d3cc74f1";
constexpr std::array<ObjectType, 7> kTypes{ObjectType::Unit, ObjectType::Ability, ObjectType::Item,
    ObjectType::Buff, ObjectType::Upgrade, ObjectType::Doodad, ObjectType::Destructable};
constexpr std::array<std::string_view, 7> kNames{"unit", "ability", "item", "buff", "upgrade", "doodad", "destructable"};
constexpr std::size_t kObjects = 100000, kValues = 2000000, kStringBytes = 64 * 1024 * 1024;
constexpr std::size_t kSingleString = 1024 * 1024;

struct Rejection { std::string code, message, path; };
[[noreturn]] void Reject(std::string code, std::string message, std::string path = {}) {
    throw Rejection{std::move(code), std::move(message), std::move(path)};
}
bool Hash(std::string_view text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}
bool Source(const SlkSessionSourceIdentity& source) {
    return (source.presence == SlkSessionSourcePresence::Present && Hash(source.sha256)) ||
        (source.presence == SlkSessionSourcePresence::Missing && source.sha256.empty());
}
bool BindingsValid(const SlkSessionBindings& value) {
    if (!Hash(value.candidateSha256) || !Hash(value.orderedTemplatesSha256) || !Hash(value.materializerSha256) ||
        !Hash(value.slkLibrarySha256) || !Hash(value.frontendModelSha256) || !Hash(value.metadataSha256) ||
        value.dataSetIdentity.empty() || value.dataSetIdentity.size() > 256 ||
        value.dataSetIdentity.find('\0') != std::string::npos || !Source(value.strings) ||
        value.mapInfo.presence != SlkSessionSourcePresence::Present || !Source(value.mapInfo)) return false;
    for (std::size_t i = 0; i < kTypes.size(); ++i) {
        if (!Source(value.nativeMembers[i]) || !Hash(value.defaultsSha256[i])) return false;
    }
    return true;
}
bool AuditedLibrary(std::string_view hash) {
    return hash.size() == kLibrarySha256.size() && std::equal(hash.begin(), hash.end(), kLibrarySha256.begin(), [](char a, char b) {
        return (a >= 'A' && a <= 'F' ? static_cast<char>(a - 'A' + 'a') : a) == b;
    });
}
bool Rawcode(std::string_view id) {
    return id.size() == 4 && std::all_of(id.begin(), id.end(), [](unsigned char c) { return c >= 0x21 && c <= 0x7e; });
}
bool StaticId(std::string_view id) {
    return id.size() == 4 && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    });
}
bool TriggerString(std::string_view text) {
    std::size_t at = 0;
    while ((at = text.find("TRIGSTR_", at)) != std::string_view::npos) {
        at += 8;
        if (at < text.size() && text[at] >= '0' && text[at] <= '9') return true;
    }
    return false;
}
struct Budget {
    std::size_t objects = 0, values = 0, strings = 0;
    void Spend(std::size_t& count, std::size_t amount, std::size_t maximum, const std::string& path) {
        if (count > maximum || amount > maximum - count) Reject("SLK_SESSION_LIMIT", "完整会话超过比较预算。", path);
        count += amount;
    }
    void Text(std::string_view value, std::size_t maximum, const std::string& path) {
        if (value.size() > maximum || value.find('\0') != std::string_view::npos)
            Reject("SLK_SESSION_STRING_UNSUPPORTED", "字符串过长或包含 NUL。", path);
        Spend(strings, value.size(), kStringBytes, path);
    }
};
const SlkSessionScalar* Scalar(const SlkSessionObject& object, std::string_view field) {
    const auto found = object.fields.find(std::string(field));
    return found == object.fields.end() ? nullptr : std::get_if<SlkSessionScalar>(&found->second);
}
const std::string* Text(const SlkSessionObject& object, std::string_view field) {
    const auto* value = Scalar(object, field);
    return value ? std::get_if<std::string>(value) : nullptr;
}
bool True(const SlkSessionObject& object, std::string_view field) {
    const auto* value = Scalar(object, field);
    const auto* flag = value ? std::get_if<bool>(value) : nullptr;
    return flag && *flag;
}
bool Generated(const SlkSessionObject& object, const std::string& path) {
    const auto found = object.fields.find("w2lobject");
    if (found == object.fields.end()) return false;
    const auto* scalar = std::get_if<SlkSessionScalar>(&found->second);
    if (scalar && std::holds_alternative<std::string>(*scalar)) return true; // 包括 Lua truthy 空字符串
    const auto* flag = scalar ? std::get_if<bool>(scalar) : nullptr;
    if (flag && !*flag) return false;
    Reject("SLK_SESSION_MARKER_UNSUPPORTED", "truthy W2lo 不是字符串；真实 mark_obj 无法对其调用 find。", path);
}

using Metadata = std::map<std::string, SlkSessionFieldDefinition>;
using Objects = std::map<std::string, const SlkSessionObject*>;
SlkSessionFieldType ScalarType(const SlkSessionScalar& value) {
    if (std::holds_alternative<std::int64_t>(value) || std::holds_alternative<double>(value)) return SlkSessionFieldType::Number;
    if (std::holds_alternative<std::string>(value)) return SlkSessionFieldType::String;
    return SlkSessionFieldType::Boolean;
}
std::optional<SlkSessionFieldType> InternalType(std::string_view field) {
    if (field == "_id" || field == "_parent" || field == "_type" || field == "_code") return SlkSessionFieldType::String;
    if (field == "_obj" || field == "_create") return SlkSessionFieldType::Boolean;
    if (field == "_max_level") return SlkSessionFieldType::Number;
    return std::nullopt;
}
void ValidateScalar(const SlkSessionScalar& value, SlkSessionFieldType expected, bool created,
    Budget& budget, const std::string& path) {
    budget.Spend(budget.values, 1, kValues, path);
    if (expected == SlkSessionFieldType::Unknown || ScalarType(value) != expected)
        Reject("SLK_SESSION_FIELD_TYPE", "字段值与已解析 metadata 的 Lua 类型不一致。", path);
    if (const auto* real = std::get_if<double>(&value); real && !std::isfinite(*real))
        Reject("SLK_SESSION_NUMBER_UNSUPPORTED", "不接受 NaN 或无穷值。", path);
    if (const auto* text = std::get_if<std::string>(&value)) {
        budget.Text(*text, kSingleString, path);
        if (created && TriggerString(*text))
            Reject("SLK_SESSION_WTS_UNPROVEN", "new 字面 TRIGSTR 的语义未覆盖；不能对两侧统一解析 WTS。", path);
    }
}
void ValidateObject(const SlkSessionObject& object, std::string_view type, const Metadata& metadata,
    bool created, Budget& budget, const std::string& path) {
    budget.Spend(budget.objects, 1, kObjects, path);
    if (!Rawcode(object.id)) Reject("SLK_SESSION_OBJECT_ID", "对象 ID 必须为四个可打印 ASCII 字节。", path);
    for (const auto& [name, value] : object.fields) {
        budget.Text(name, 256, path);
        if (name.empty()) Reject("SLK_SESSION_METADATA_UNPROVEN", "字段名为空。", path);
        const auto fieldPath = path + "/" + name;
        if (name == "w2lobject") {
            (void)Generated(object, fieldPath);
            const auto* scalar = std::get_if<SlkSessionScalar>(&value);
            ValidateScalar(*scalar, ScalarType(*scalar), created, budget, fieldPath);
            continue;
        }
        const auto internal = InternalType(name);
        const auto definition = metadata.find(name);
        if (!internal && definition == metadata.end())
            Reject("SLK_SESSION_METADATA_UNPROVEN", "完整对象包含未解析字段。", fieldPath);
        const bool array = !internal && definition->second.array;
        const auto expected = internal ? *internal : definition->second.type;
        if (array) {
            const auto* values = std::get_if<std::vector<SlkSessionScalar>>(&value);
            if (!values) Reject("SLK_SESSION_FIELD_TYPE", "重复字段不是稠密一维数组。", fieldPath);
            budget.Spend(budget.values, 1, kValues, fieldPath);
            for (const auto& item : *values) ValidateScalar(item, expected, created, budget, fieldPath);
        } else {
            const auto* scalar = std::get_if<SlkSessionScalar>(&value);
            if (!scalar) Reject("SLK_SESSION_FIELD_TYPE", "标量字段不能用数组替代。", fieldPath);
            ValidateScalar(*scalar, expected, created, budget, fieldPath);
        }
    }
    const auto* id = Text(object, "_id");
    const auto* objectType = Text(object, "_type");
    const auto* code = Text(object, "_code");
    if (!id || *id != object.id || !objectType || *objectType != type || !code || !Rawcode(*code))
        Reject("SLK_SESSION_OBJECT_INCOMPLETE", "完整对象缺少一致的 _id/_type/_code。", path);
    if (created && (!StaticId(object.id) || !True(object, "_obj") || !True(object, "_create") ||
        !Text(object, "w2lobject") || *Text(object, "w2lobject") != "static"))
        Reject("SLK_SESSION_CREATION_UNPROVEN", "仅支持具有真实内部标记的静态 new 对象。", path);
}
Objects Index(const std::vector<SlkSessionObject>& objects, std::string_view type, const Metadata& metadata,
    bool created, Budget& budget, const std::string& path) {
    if (objects.size() > kObjects - budget.objects) Reject("SLK_SESSION_LIMIT", "完整会话超过对象预算。", path);
    Objects result;
    for (const auto& object : objects) {
        const auto objectPath = path + "/" + object.id;
        ValidateObject(object, type, metadata, created, budget, objectPath);
        if (!result.emplace(object.id, &object).second)
            Reject("SLK_SESSION_DUPLICATE_OBJECT", "对象 ID 重复；不能用最后一个覆盖前面的记录。", objectPath);
    }
    return result;
}
bool EqualScalar(const SlkSessionScalar& expected, const SlkSessionScalar& actual, const std::string& path) {
    if (ScalarType(expected) != ScalarType(actual)) return false; // Lua type(v) 比较
    if (expected.index() != actual.index())
        Reject("SLK_SESSION_ROUNDING_UNPROVEN", "integer/float 的 around_number 比较尚未验证。", path);
    if (const auto* number = std::get_if<double>(&expected)) {
        // 同一有限输入经过目标 around_number 必得同一结果；不同 double 不猜四位舍入。
        if (*number != std::get<double>(actual))
            Reject("SLK_SESSION_ROUNDING_UNPROVEN", "不同 double 的 around_number 等价性尚未验证。", path);
        return true;
    }
    return expected == actual;
}
bool EqualValue(const SlkSessionValue& expected, const SlkSessionValue& actual, const std::string& path) {
    if (expected.index() != actual.index()) return false;
    if (const auto* scalar = std::get_if<SlkSessionScalar>(&expected))
        return EqualScalar(*scalar, std::get<SlkSessionScalar>(actual), path);
    const auto& left = std::get<std::vector<SlkSessionScalar>>(expected);
    const auto& right = std::get<std::vector<SlkSessionScalar>>(actual);
    if (left.size() > right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (!EqualScalar(left[i], right[i], path + "/" + std::to_string(i + 1))) return false;
    }
    return true;
}
bool EqualObject(const SlkSessionObject& expected, const SlkSessionObject& actual, const std::string& path) {
    for (const auto& [field, value] : expected.fields) {
        if (field == "_create") continue;
        const auto found = actual.fields.find(field);
        if (found == actual.fields.end() || !EqualValue(value, found->second, path + "/" + field)) return false;
    }
    return true;
}
void DefaultCoverage(const SlkSessionObject& object, const Objects& defaults, bool created, const std::string& path) {
    const auto* parent = Text(object, "_parent");
    if (!parent) {
        if (created) Reject("SLK_SESSION_DEFAULT_UNPROVEN", "new 对象缺少标准父 ID。", path);
        return;
    }
    const auto found = defaults.find(*parent);
    if (found == defaults.end()) Reject("SLK_SESSION_DEFAULT_UNPROVEN", "父对象不在完整标准 default 中。", path);
    const auto& base = *found->second;
    if (*Text(object, "_code") != *Text(base, "_code"))
        Reject("SLK_SESSION_DEFAULT_UNPROVEN", "对象 base code 与标准父对象不一致。", path);
    for (const auto& [field, value] : base.fields) {
        (void)value;
        if (field != "_create" && !object.fields.contains(field))
            Reject("SLK_SESSION_OBJECT_INCOMPLETE", "完整对象缺少标准 default 字段 " + field + "。", path);
    }
}
Diagnostic DiagnosticFor(const Rejection& failure, const SlkSessionBindings& bindings) {
    return {DiagnosticSeverity::Error, "Script.SlkSession", failure.code, failure.message,
        OriginSpan{failure.path.empty() ? "slk-session" : failure.path, 0, 0, bindings.candidateSha256}};
}
} // namespace

Result<SlkTemplateSessionProof> ProveSlkTemplateSessionNoObjectWrites(const SlkTemplateSession& session) {
    try {
        if (!BindingsValid(session.bindings)) Reject("SLK_SESSION_BINDINGS_INCOMPLETE", "候选、模板、工具、元数据、默认值或七类成员身份缺失。");
        if (!AuditedLibrary(session.bindings.slkLibrarySha256)) Reject("SLK_SESSION_MODEL_UNSUPPORTED", "SLK 生命周期源码身份不是已审计版本。");
        if (!session.completeStaticCreationSession) Reject("SLK_SESSION_SCOPE_UNPROVEN", "尚未证明全部模板及副作用范围闭合。");
        if (!session.unresolvedSemantics.empty() || !session.materializationDiagnostics.empty())
            Reject("SLK_SESSION_MATERIALIZATION_UNPROVEN", "物化过程仍有未覆盖语义或诊断，不能证明无写入。");
        Budget budget;
        SlkTemplateSessionProof proof;
        proof.bindings = session.bindings;
        std::vector<Diagnostic> changes;
        for (std::size_t i = 0; i < kTypes.size(); ++i) {
            const auto& input = session.classes[i];
            const std::string path = "slk-session/" + std::string(kNames[i]);
            if (input.type != kTypes[i]) Reject("SLK_SESSION_CLASS_COVERAGE", "七类对象必须按规定顺序各提供一次。", path);
            if (input.materialization != SlkSessionMaterialization::CompleteDefaultsMetadataOverlayAndOrderedWrites)
                Reject("SLK_SESSION_MATERIALIZATION_UNPROVEN", "对象图未证明含完整 default、metadata、overlay 和有序写入。", path);
            Metadata metadata;
            for (const auto& field : input.metadata) {
                budget.Text(field.name, 256, path);
                if (field.name.empty() || InternalType(field.name) || field.name == "w2lobject" ||
                    (field.type != SlkSessionFieldType::Number && field.type != SlkSessionFieldType::String && field.type != SlkSessionFieldType::Boolean) ||
                    !metadata.emplace(field.name, field).second)
                    Reject("SLK_SESSION_METADATA_UNPROVEN", "字段 metadata 未知、重复或与内部字段冲突。", path);
                budget.Spend(budget.values, 1, kValues, path);
            }
            const auto defaults = Index(input.standardDefaults, kNames[i], metadata, false, budget, path + "/default");
            const auto existing = Index(input.existingObjects, kNames[i], metadata, false, budget, path + "/frontend");
            const auto created = Index(input.createdObjects, kNames[i], metadata, true, budget, path + "/new");
            for (const auto& [id, object] : defaults) {
                if (Generated(*object, path) || Text(*object, "_parent"))
                    Reject("SLK_SESSION_DEFAULT_UNPROVEN", "标准 default 不能是 generated 或 custom 父链。", path + "/default/" + id);
                if (!existing.contains(id)) Reject("SLK_SESSION_OBJECT_INCOMPLETE", "frontend 缺少标准 default 对象。", path + "/frontend/" + id);
            }
            Objects old;
            for (const auto& [id, object] : existing) {
                DefaultCoverage(*object, defaults, false, path + "/frontend/" + id);
                if (Generated(*object, path + "/frontend/" + id)) old.emplace(id, object);
            }
            auto& record = proof.classes[i];
            record.type = input.type;
            record.existingObjectCount = existing.size();
            record.generatedObjectCount = old.size();
            for (const auto& [id, object] : created) {
                const auto objectPath = path + "/new/" + id;
                DefaultCoverage(*object, defaults, true, objectPath);
                if (defaults.contains(id) || (existing.contains(id) && !old.contains(id)))
                    Reject("SLK_SESSION_CREATION_COLLISION", "静态 new 与标准对象或未移入 old 的现有对象冲突。", objectPath);
                const auto previous = old.find(id);
                if (previous == old.end()) { ++record.remainingNew; continue; }
                if (!EqualObject(*object, *previous->second, objectPath)) ++record.changed;
                else ++record.matchedObjectCount;
                old.erase(previous);
            }
            record.remainingOld = old.size();
            if (record.remainingNew || record.remainingOld || record.changed) {
                changes.push_back(DiagnosticFor({"SLK_SESSION_REQUIRES_OBJECT_WRITES",
                    std::string(kNames[i]) + " 需要整类物化：new=" + std::to_string(record.remainingNew) +
                    ", old=" + std::to_string(record.remainingOld) + ", change=" + std::to_string(record.changed) + "。", path}, session.bindings));
            }
        }
        if (!changes.empty()) { Result<SlkTemplateSessionProof> result; result.diagnostics = std::move(changes); return result; }
        proof.coverage = SlkSessionProofCoverage::WholeSessionNoObjectWrites;
        return Result<SlkTemplateSessionProof>::Success(std::move(proof));
    } catch (const Rejection& failure) {
        Result<SlkTemplateSessionProof> result;
        result.diagnostics.push_back(DiagnosticFor(failure, session.bindings));
        return result;
    } catch (const std::exception&) {
        return Result<SlkTemplateSessionProof>::Failure("Script.SlkSession", "SLK_SESSION_RESOURCE_FAILURE", "完整会话比较未完成，未生成凭据。");
    }
}

bool SlkTemplateSessionProofMatches(const SlkTemplateSessionProof& proof, const SlkSessionBindings& bindings) noexcept {
    if (proof.coverage != SlkSessionProofCoverage::WholeSessionNoObjectWrites || !BindingsValid(bindings) ||
        !AuditedLibrary(bindings.slkLibrarySha256) || proof.bindings != bindings) return false;
    for (std::size_t i = 0; i < kTypes.size(); ++i) {
        const auto& item = proof.classes[i];
        if (item.type != kTypes[i] || item.remainingNew || item.remainingOld || item.changed ||
            item.generatedObjectCount != item.matchedObjectCount) return false;
    }
    return true;
}
} // namespace War3Map::Script
