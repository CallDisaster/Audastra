#include "War3MapCore/Script/SlkTemplateProof.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace War3Map::Script {
namespace {
using namespace ObjectData;

bool Rawcode(std::string_view value) {
    return value.size() == 4 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return c >= 0x21 && c <= 0x7e;
    });
}

Result<SlkTemplateProof> Failure(std::string code, std::string message,
    const SlkTemplateSource& source = {}) {
    auto result = Result<SlkTemplateProof>::Failure("Script.SlkProof", std::move(code),
        "第 " + std::to_string(source.line) + " 行，第 " + std::to_string(source.column) + " 列：" + message);
    result.diagnostics.front().origin = OriginSpan{"slk-template", source.offset, source.length, {}};
    return result;
}

Result<Value> TypedValue(const SlkTemplateScalar& scalar, const ObjectFieldDefinition& field) {
    const auto fail = [] {
        return Result<Value>::Failure("Script.SlkProof", "SLK_PROOF_VALUE_UNSUPPORTED",
            "模板值不能无歧义转换为 metadata 声明的原生类型。");
    };
    if (!field.nativeValueType) return fail();
    if (*field.nativeValueType == NativeValueType::String) {
        const auto* text = std::get_if<std::string>(&scalar);
        if (!text || text->find('\0') != std::string::npos) return fail();
        return Result<Value>::Success(*text);
    }
    if (const auto* flag = std::get_if<bool>(&scalar)) {
        if (*field.nativeValueType != NativeValueType::Int32 || Text::LowerAscii(field.valueType) != "bool")
            return fail();
        return Result<Value>::Success(std::int32_t{*flag ? 1 : 0});
    }
    double value;
    if (const auto* integer = std::get_if<std::int64_t>(&scalar)) {
        if (*integer < -9007199254740991LL || *integer > 9007199254740991LL) return fail();
        value = static_cast<double>(*integer);
    } else if (const auto* real = std::get_if<double>(&scalar)) value = *real;
    else return fail();
    if (!std::isfinite(value)) return fail();
    if (*field.nativeValueType == NativeValueType::Int32) {
        if (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max()
            || std::trunc(value) != value) return fail();
        if (Text::LowerAscii(field.valueType) == "bool" && value != 0 && value != 1) return fail();
        return Result<Value>::Success(static_cast<std::int32_t>(value));
    }
    if (std::abs(value) > std::numeric_limits<float>::max()) return fail();
    const float real = static_cast<float>(value);
    if (!std::isfinite(real) || (value != 0 && real == 0)) return fail();
    return Result<Value>::Success(real);
}

using FieldAddress = std::tuple<std::string, std::uint32_t, std::uint32_t>;
struct ExpectedField {
    ModificationKey key;
    NativeValueType valueType;
    Value value;
    SlkTemplateSource source;
};

} // namespace

Result<SlkTemplateProof> ProveSlkTemplateExplicitAssignments(
    const SlkTemplatePlan& plan, const NativeObjectDocument& abilities,
    const ObjectFieldSchema& schema, const MapData::StringTableDocument& strings,
    const SlkTemplateProofOptions& options) {
    if (abilities.Type() != ObjectType::Ability || !abilities.IsStructurallySafe() || !abilities.IsEditable())
        return Failure("SLK_PROOF_DOCUMENT_UNSAFE", "需要结构完整、身份无歧义的技能 native 文档。");
    const SlkTemplatePlanLimits limits;
    if (plan.abilities.size() > limits.abilities)
        return Failure("SLK_PROOF_LIMIT", "技能计划数量超过校验上限。");
    if (!plan.abilities.empty() && !options.resolveStandardParent)
        return Failure("SLK_PROOF_PARENT_UNRESOLVED", "缺少标准父对象目录解析器。", plan.abilities.front().source);

    SlkTemplateProof proof;
    std::set<std::string> ids;
    for (const auto& ability : plan.abilities) {
        if (!Rawcode(ability.id) || !Rawcode(ability.parentId) || ability.id == ability.parentId || !ids.insert(ability.id).second)
            return Failure("SLK_PROOF_OBJECT_ID_INVALID", "计划对象 ID 无效、与父 ID 相同或重复。", ability.source);
        if (ability.writes.size() > limits.fieldWrites - proof.assignmentCount)
            return Failure("SLK_PROOF_LIMIT", "字段赋值数量超过校验上限。", ability.source);
        const ObjectKey key{ObjectType::Ability, ability.id};
        const auto* object = abilities.FindObject(key);
        if (!object)
            return Failure("SLK_PROOF_OBJECT_MISSING", "缺少计划对象 " + ability.id + "。", ability.source);
        if (!object->parentRawcode || *object->parentRawcode != ability.parentId)
            return Failure("SLK_PROOF_PARENT_MISMATCH", "对象 " + ability.id + " 的 custom 父 ID 不一致。", ability.source);
        if (const auto* parent = abilities.FindObject({ObjectType::Ability, ability.parentId})) {
            return Failure("SLK_PROOF_PARENT_OVERLAY_UNSUPPORTED", parent->parentRawcode
                ? "暂不证明自定义父链继承。" : "暂不证明地图中的父对象覆盖。", ability.source);
        }
        const auto parent = options.resolveStandardParent(ability.parentId);
        if (!parent.Succeeded() || parent.value->rawcode != ability.parentId || !Rawcode(parent.value->baseCode))
            return Failure("SLK_PROOF_PARENT_UNRESOLVED", "不能确定标准父对象 " + ability.parentId + " 及其 SLK code。", ability.source);

        std::map<FieldAddress, ExpectedField> expected;
        for (const auto& write : ability.writes) {
            const auto match = schema.MatchSourceField(ObjectType::Ability, write.field, *parent.value);
            if (!match.definition || !match.safeToEdit || !match.definition->nativeValueType ||
                !Rawcode(match.modification.fieldId) || !match.modification.level || !match.modification.dataIndex ||
                match.modification.variation || (match.sourceIndex && *match.sourceIndex != 0)) {
                return Failure("SLK_PROOF_FIELD_UNRESOLVED", "字段 " + write.field + " 无唯一可靠的标量 native 地址：" + match.reason,
                    write.source);
            }
            // 反向匹配也须保持精确 level/data，不能用某个相似字段填补 metadata 缺口。
            const auto reverse = schema.MatchNativeField(ObjectType::Ability, match.modification, *parent.value);
            if (!reverse.safeToEdit || reverse.definition != match.definition || reverse.modification != match.modification)
                return Failure("SLK_PROOF_FIELD_UNRESOLVED", "字段 " + write.field + " 不能通过 metadata 地址反向校验。", write.source);
            const auto typed = TypedValue(write.value, *match.definition);
            if (!typed.Succeeded())
                return Failure("SLK_PROOF_VALUE_UNSUPPORTED", "字段 " + write.field + " 的值与 metadata 原生类型不兼容。", write.source);
            if (const auto* text = std::get_if<std::string>(&*typed.value); text && !strings.Resolve(*text).Succeeded())
                return Failure("SLK_PROOF_WTS_UNRESOLVED", "字段 " + write.field + " 的模板 WTS 引用无效、缺失或重复。", write.source);
            const FieldAddress address{match.modification.fieldId, *match.modification.level, *match.modification.dataIndex};
            expected.insert_or_assign(address, ExpectedField{match.modification, *match.definition->nativeValueType, *typed.value, write.source});
            ++proof.assignmentCount;
        }
        for (const auto& [address, field] : expected) {
            (void)address;
            const auto found = std::find_if(object->modifications.begin(), object->modifications.end(),
                [&](const ObjectModification& modification) { return modification.key == field.key; });
            if (found == object->modifications.end())
                return Failure("SLK_PROOF_INHERITED_FIELD_UNSUPPORTED", "对象 " + ability.id + " 缺少显式字段 " + field.key.fieldId +
                    "；不根据父对象继承推定等价。", field.source);
            const auto storedType = abilities.ModificationValueType(key, field.key);
            if (!storedType || *storedType != field.valueType || found->valueWasOpaque)
                return Failure("SLK_PROOF_NATIVE_TYPE_MISMATCH", "对象 " + ability.id + " 字段 " + field.key.fieldId +
                    " 的实际 native 存储类型与 metadata 不一致。", field.source);
            bool same = false;
            if (const auto* expectedInteger = std::get_if<std::int32_t>(&field.value)) {
                const auto* actual = std::get_if<std::int32_t>(&found->value);
                same = actual && *actual == *expectedInteger;
            } else if (const auto* expectedReal = std::get_if<float>(&field.value)) {
                const auto* actual = std::get_if<float>(&found->value);
                same = actual && std::isfinite(*actual) &&
                    std::bit_cast<std::uint32_t>(*actual) == std::bit_cast<std::uint32_t>(*expectedReal);
            } else if (const auto* expectedText = std::get_if<std::string>(&field.value)) {
                const auto* actual = std::get_if<std::string>(&found->value);
                if (!actual)
                    return Failure("SLK_PROOF_NATIVE_TYPE_MISMATCH", "native 字符串缺少可靠的字符串值。", field.source);
                const auto resolvedExpected = strings.Resolve(*expectedText);
                const auto resolvedActual = strings.Resolve(*actual);
                if (!resolvedExpected.Succeeded() || !resolvedActual.Succeeded())
                    return Failure("SLK_PROOF_WTS_UNRESOLVED", "字段 " + field.key.fieldId + " 的 WTS 引用无效、缺失或重复。", field.source);
                same = *resolvedExpected.value == *resolvedActual.value;
            }
            if (!same)
                return Failure("SLK_PROOF_VALUE_MISMATCH", "对象 " + ability.id + " 字段 " + field.key.fieldId +
                    " 的实际值与模板最终赋值不一致。", field.source);
            proof.fields.push_back({key, field.key, field.valueType, field.source});
        }
        ++proof.abilityCount;
    }
    return Result<SlkTemplateProof>::Success(std::move(proof));
}

} // namespace War3Map::Script
