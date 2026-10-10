// 往返兼容性验证测试：确保新增/修改/删除操作后的序列化结果可被正确重新解析，
// 且格式与 World Editor / YDWE 完全兼容。

#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace War3Map;
using namespace War3Map::ObjectData;

int g_failures = 0;

void Check(bool condition, std::string_view message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void AppendInt32(std::vector<std::byte>& output, std::int32_t value) {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    output.push_back(static_cast<std::byte>(bits & 0xffu));
    output.push_back(static_cast<std::byte>((bits >> 8u) & 0xffu));
    output.push_back(static_cast<std::byte>((bits >> 16u) & 0xffu));
    output.push_back(static_cast<std::byte>((bits >> 24u) & 0xffu));
}

void AppendFloat(std::vector<std::byte>& output, float value) {
    AppendInt32(output, std::bit_cast<std::int32_t>(std::bit_cast<std::uint32_t>(value)));
}

void AppendId(std::vector<std::byte>& output, std::string_view value) {
    for (const char character : value) {
        output.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
}

void AppendZeroId(std::vector<std::byte>& output) {
    output.insert(output.end(), 4, std::byte{0});
}

void AppendCString(std::vector<std::byte>& output, std::string_view value) {
    for (const char character : value) {
        output.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    output.push_back(std::byte{0});
}

[[nodiscard]] bool HasLevelData(ObjectType type) {
    return type == ObjectType::Ability
        || type == ObjectType::Upgrade
        || type == ObjectType::Doodad;
}

// 构建一个最小但完整的 native fixture：1 个原始对象 + 1 个自定义对象。
[[nodiscard]] std::vector<std::byte> BuildMinimalFixture(ObjectType type) {
    std::vector<std::byte> output;
    AppendInt32(output, 2); // version

    // 原始对象表：1 个对象 "hfoo"，2 个字段（不同 fieldId 避免重复）
    AppendInt32(output, 1);
    AppendId(output, "hfoo");
    AppendZeroId(output);
    AppendInt32(output, 2);

    // 字段 1: Int32 "uhpm"
    AppendId(output, "uhpm");
    AppendInt32(output, static_cast<std::int32_t>(NativeValueType::Int32));
    if (HasLevelData(type)) {
        AppendInt32(output, 0); // level
        AppendInt32(output, 0); // dataIndex
    }
    AppendInt32(output, 100);
    AppendZeroId(output);

    // 字段 2: String "unam"
    AppendId(output, "unam");
    AppendInt32(output, static_cast<std::int32_t>(NativeValueType::String));
    if (HasLevelData(type)) {
        AppendInt32(output, 0);
        AppendInt32(output, 0);
    }
    AppendCString(output, "Footman");
    AppendId(output, "hfoo");

    // 自定义对象表：1 个对象 "H001" 继承 "hfoo"
    AppendInt32(output, 1);
    AppendId(output, "hfoo");
    AppendId(output, "H001");
    AppendInt32(output, 1);

    AppendId(output, "udef");
    AppendInt32(output, static_cast<std::int32_t>(NativeValueType::Int32));
    if (HasLevelData(type)) {
        AppendInt32(output, 0);
        AppendInt32(output, 0);
    }
    AppendInt32(output, 500);
    AppendId(output, "H001");

    return output;
}

[[nodiscard]] std::unique_ptr<NativeObjectDocument> ParseNative(
    const NativeObjectCodec& codec,
    std::span<const std::byte> source) {
    CodecParseResult parsed = codec.Parse(source);
    auto* raw = dynamic_cast<NativeObjectDocument*>(parsed.document.release());
    return std::unique_ptr<NativeObjectDocument>(raw);
}

[[nodiscard]] std::unique_ptr<LniObjectDocument> ParseLni(
    const LniObjectCodec& codec,
    std::string_view source) {
    std::vector<std::byte> bytes(source.size());
    std::transform(source.begin(), source.end(), bytes.begin(), [](char c) {
        return static_cast<std::byte>(static_cast<unsigned char>(c));
    });
    CodecParseResult parsed = codec.Parse(bytes);
    auto* raw = dynamic_cast<LniObjectDocument*>(parsed.document.release());
    return std::unique_ptr<LniObjectDocument>(raw);
}

// ============================================================================
// 测试 1：Native AddModification 往返
// ============================================================================
void TestNativeAddModificationRoundtrip() {
    constexpr std::array types{
        ObjectType::Unit,
        ObjectType::Ability,
        ObjectType::Item,
        ObjectType::Buff,
        ObjectType::Upgrade,
        ObjectType::Doodad,
        ObjectType::Destructable,
    };

    for (const ObjectType type : types) {
        const std::vector<std::byte> fixture = BuildMinimalFixture(type);
        NativeObjectCodec codec(type);
        auto document = ParseNative(codec, fixture);
        Check(document != nullptr, "AddModification: parse fixture");
        if (!document) continue;
        Check(document->IsEditable(), "AddModification: document editable");

        // 向自定义对象 H001 追加一个新的 Int32 字段
        ModificationKey newField;
        newField.fieldId = "utyp";
        if (HasLevelData(type)) {
            newField.level = 0;
            newField.dataIndex = 0;
        }
        Status addStatus = document->AddModification(
            ObjectKey{type, "H001"},
            newField,
            NativeValueType::Int32,
            std::int32_t{42});
        Check(addStatus.Succeeded(), "AddModification: add new field");

        // 序列化
        auto serialized = codec.Serialize(*document);
        Check(serialized.Succeeded(), "AddModification: serialize");
        if (!serialized.value.has_value()) continue;

        // 重新解析
        auto reparsed = ParseNative(codec, *serialized.value);
        Check(reparsed != nullptr, "AddModification: reparse");
        if (!reparsed) continue;
        Check(reparsed->IsStructurallySafe(), "AddModification: reparsed safe");
        Check(reparsed->IsEditable(), "AddModification: reparsed editable");

        // 验证新字段存在且值正确
        const ObjectRecord* obj = reparsed->FindObject(ObjectKey{type, "H001"});
        Check(obj != nullptr, "AddModification: object found after reparse");
        if (!obj) continue;
        Check(obj->modifications.size() == 2, "AddModification: modification count");

        auto fieldIt = std::find_if(
            obj->modifications.begin(), obj->modifications.end(),
            [&](const ObjectModification& m) { return m.key.fieldId == "utyp"; });
        Check(fieldIt != obj->modifications.end(), "AddModification: new field found");
        if (fieldIt != obj->modifications.end()) {
            const auto* val = std::get_if<std::int32_t>(&fieldIt->value);
            Check(val != nullptr && *val == 42, "AddModification: new field value");
        }

        // 验证原有字段未受影响
        auto origField = std::find_if(
            obj->modifications.begin(), obj->modifications.end(),
            [&](const ObjectModification& m) { return m.key.fieldId == "udef"; });
        Check(origField != obj->modifications.end(), "AddModification: original field preserved");
        if (origField != obj->modifications.end()) {
            const auto* val = std::get_if<std::int32_t>(&origField->value);
            Check(val != nullptr && *val == 500, "AddModification: original field value");
        }
    }
}

// ============================================================================
// 测试 2：Native AddCustomObject 往返
// ============================================================================
void TestNativeAddCustomObjectRoundtrip() {
    constexpr std::array types{
        ObjectType::Unit,
        ObjectType::Ability,
        ObjectType::Item,
        ObjectType::Buff,
        ObjectType::Upgrade,
        ObjectType::Doodad,
        ObjectType::Destructable,
    };

    for (const ObjectType type : types) {
        const std::vector<std::byte> fixture = BuildMinimalFixture(type);
        NativeObjectCodec codec(type);
        auto document = ParseNative(codec, fixture);
        Check(document != nullptr, "AddCustomObject: parse fixture");
        if (!document) continue;

        // 新增自定义对象 "X001" 继承 "hfoo"
        Status addStatus = document->AddCustomObject("X001", "hfoo");
        Check(addStatus.Succeeded(), "AddCustomObject: add object");
        Check(document->CustomObjectCount() == 2, "AddCustomObject: custom count");

        // 向新对象追加一个字段
        ModificationKey field;
        field.fieldId = "unam";
        if (HasLevelData(type)) {
            field.level = 0;
            field.dataIndex = 0;
        }
        Status modStatus = document->AddModification(
            ObjectKey{type, "X001"},
            field,
            NativeValueType::String,
            std::string{"Custom Unit"});
        Check(modStatus.Succeeded(), "AddCustomObject: add modification to new object");

        // 序列化
        auto serialized = codec.Serialize(*document);
        Check(serialized.Succeeded(), "AddCustomObject: serialize");
        if (!serialized.value.has_value()) continue;

        // 重新解析
        auto reparsed = ParseNative(codec, *serialized.value);
        Check(reparsed != nullptr, "AddCustomObject: reparse");
        if (!reparsed) continue;
        Check(reparsed->IsStructurallySafe(), "AddCustomObject: reparsed safe");
        Check(reparsed->IsEditable(), "AddCustomObject: reparsed editable");
        Check(reparsed->CustomObjectCount() == 2, "AddCustomObject: reparsed custom count");

        // 验证新对象存在
        const ObjectRecord* obj = reparsed->FindObject(ObjectKey{type, "X001"});
        Check(obj != nullptr, "AddCustomObject: new object found");
        if (!obj) continue;
        Check(obj->parentRawcode == "hfoo", "AddCustomObject: parent rawcode");
        Check(obj->modifications.size() == 1, "AddCustomObject: new object modifications");
        if (!obj->modifications.empty()) {
            const auto* val = std::get_if<std::string>(&obj->modifications[0].value);
            Check(val != nullptr && *val == "Custom Unit", "AddCustomObject: new object field value");
        }

        // 验证原有对象未受影响
        const ObjectRecord* origObj = reparsed->FindObject(ObjectKey{type, "H001"});
        Check(origObj != nullptr, "AddCustomObject: original custom object preserved");
        const ObjectRecord* stdObj = reparsed->FindObject(ObjectKey{type, "hfoo"});
        Check(stdObj != nullptr, "AddCustomObject: standard object preserved");
    }
}

// ============================================================================
// 测试 3：Native DeleteCustomObject 往返
// ============================================================================
void TestNativeDeleteCustomObjectRoundtrip() {
    constexpr std::array types{
        ObjectType::Unit,
        ObjectType::Ability,
        ObjectType::Item,
        ObjectType::Buff,
        ObjectType::Upgrade,
        ObjectType::Doodad,
        ObjectType::Destructable,
    };

    for (const ObjectType type : types) {
        const std::vector<std::byte> fixture = BuildMinimalFixture(type);
        NativeObjectCodec codec(type);
        auto document = ParseNative(codec, fixture);
        Check(document != nullptr, "DeleteCustomObject: parse fixture");
        if (!document) continue;
        Check(document->CustomObjectCount() == 1, "DeleteCustomObject: initial custom count");

        // 删除自定义对象 H001
        Status delStatus = document->DeleteCustomObject(ObjectKey{type, "H001"});
        Check(delStatus.Succeeded(), "DeleteCustomObject: delete");
        Check(document->CustomObjectCount() == 0, "DeleteCustomObject: custom count after delete");

        // 不允许删除原始对象
        Status delOrig = document->DeleteCustomObject(ObjectKey{type, "hfoo"});
        Check(!delOrig.Succeeded(), "DeleteCustomObject: cannot delete original");

        // 序列化
        auto serialized = codec.Serialize(*document);
        Check(serialized.Succeeded(), "DeleteCustomObject: serialize");
        if (!serialized.value.has_value()) continue;

        // 重新解析
        auto reparsed = ParseNative(codec, *serialized.value);
        Check(reparsed != nullptr, "DeleteCustomObject: reparse");
        if (!reparsed) continue;
        Check(reparsed->IsStructurallySafe(), "DeleteCustomObject: reparsed safe");
        Check(reparsed->CustomObjectCount() == 0, "DeleteCustomObject: reparsed custom count");
        Check(reparsed->FindObject(ObjectKey{type, "H001"}) == nullptr, "DeleteCustomObject: object gone");
        Check(reparsed->FindObject(ObjectKey{type, "hfoo"}) != nullptr, "DeleteCustomObject: standard object remains");
    }
}

// ============================================================================
// 测试 4：Native 组合操作往返（新增对象 + 追加字段 + 修改既有字段）
// ============================================================================
void TestNativeCombinedOperationsRoundtrip() {
    const ObjectType type = ObjectType::Unit;
    const std::vector<std::byte> fixture = BuildMinimalFixture(type);
    NativeObjectCodec codec(type);
    auto document = ParseNative(codec, fixture);
    Check(document != nullptr, "Combined: parse");
    if (!document) return;

    // 1. 修改原始对象的既有 Int32 字段
    ModificationKey existingKey;
    existingKey.fieldId = "uhpm";
    Status setStatus = document->SetModification(
        ObjectKey{type, "hfoo"},
        existingKey,
        std::int32_t{200});
    Check(setStatus.Succeeded(), "Combined: set existing field");

    // 2. 新增自定义对象
    Status addObjStatus = document->AddCustomObject("N001", "hfoo");
    Check(addObjStatus.Succeeded(), "Combined: add custom object");

    // 3. 向新对象追加字段
    ModificationKey newField;
    newField.fieldId = "uhpm";
    Status addModStatus = document->AddModification(
        ObjectKey{type, "N001"},
        newField,
        NativeValueType::Int32,
        std::int32_t{999});
    Check(addModStatus.Succeeded(), "Combined: add modification to new object");

    // 4. 向既有自定义对象追加字段
    ModificationKey extraField;
    extraField.fieldId = "utyp";
    Status addMod2 = document->AddModification(
        ObjectKey{type, "H001"},
        extraField,
        NativeValueType::Int32,
        std::int32_t{77});
    Check(addMod2.Succeeded(), "Combined: add modification to existing custom");

    // 序列化
    auto serialized = codec.Serialize(*document);
    Check(serialized.Succeeded(), "Combined: serialize");
    if (!serialized.value.has_value()) return;

    // 重新解析并验证全部操作
    auto reparsed = ParseNative(codec, *serialized.value);
    Check(reparsed != nullptr, "Combined: reparse");
    if (!reparsed) return;
    Check(reparsed->IsStructurallySafe(), "Combined: reparsed safe");
    Check(reparsed->IsEditable(), "Combined: reparsed editable");
    Check(reparsed->OriginalObjectCount() == 1, "Combined: original count");
    Check(reparsed->CustomObjectCount() == 2, "Combined: custom count");

    // 验证修改后的字段
    const ObjectRecord* origObj = reparsed->FindObject(ObjectKey{type, "hfoo"});
    Check(origObj != nullptr, "Combined: original object found");
    if (origObj) {
        auto hpField = std::find_if(
            origObj->modifications.begin(), origObj->modifications.end(),
            [](const ObjectModification& m) { return m.key.fieldId == "uhpm"; });
        Check(hpField != origObj->modifications.end(), "Combined: uhpm field found");
        if (hpField != origObj->modifications.end()) {
            const auto* val = std::get_if<std::int32_t>(&hpField->value);
            Check(val != nullptr && *val == 200, "Combined: modified field value");
        }
    }

    // 验证新对象
    const ObjectRecord* newObj = reparsed->FindObject(ObjectKey{type, "N001"});
    Check(newObj != nullptr, "Combined: new object found");
    if (newObj) {
        Check(newObj->parentRawcode == "hfoo", "Combined: new object parent");
        Check(newObj->modifications.size() == 1, "Combined: new object mod count");
        if (!newObj->modifications.empty()) {
            const auto* val = std::get_if<std::int32_t>(&newObj->modifications[0].value);
            Check(val != nullptr && *val == 999, "Combined: new object field value");
        }
    }

    // 验证既有自定义对象的新字段
    const ObjectRecord* h001 = reparsed->FindObject(ObjectKey{type, "H001"});
    Check(h001 != nullptr, "Combined: H001 found");
    if (h001) {
        Check(h001->modifications.size() == 2, "Combined: H001 mod count");
        auto defField = std::find_if(
            h001->modifications.begin(), h001->modifications.end(),
            [](const ObjectModification& m) { return m.key.fieldId == "utyp"; });
        Check(defField != h001->modifications.end(), "Combined: H001 new field found");
        if (defField != h001->modifications.end()) {
            const auto* val = std::get_if<std::int32_t>(&defField->value);
            Check(val != nullptr && *val == 77, "Combined: H001 new field value");
        }
    }

    // 二次往返：确保序列化结果再次序列化后字节一致
    auto serialized2 = codec.Serialize(*reparsed);
    Check(serialized2.Succeeded(), "Combined: second serialize");
    if (serialized2.value.has_value()) {
        Check(*serialized2.value == *serialized.value, "Combined: byte-stable second roundtrip");
    }
}

// ============================================================================
// 测试 5：Native AddModification 字符串类型往返
// ============================================================================
void TestNativeAddStringModificationRoundtrip() {
    const ObjectType type = ObjectType::Unit;
    const std::vector<std::byte> fixture = BuildMinimalFixture(type);
    NativeObjectCodec codec(type);
    auto document = ParseNative(codec, fixture);
    Check(document != nullptr, "AddStringMod: parse");
    if (!document) return;

    ModificationKey strField;
    strField.fieldId = "utip";
    Status addStatus = document->AddModification(
        ObjectKey{type, "H001"},
        strField,
        NativeValueType::String,
        std::string{"Custom Tooltip \xe4\xb8\xad\xe6\x96\x87"});
    Check(addStatus.Succeeded(), "AddStringMod: add string field");

    auto serialized = codec.Serialize(*document);
    Check(serialized.Succeeded(), "AddStringMod: serialize");
    if (!serialized.value.has_value()) return;

    auto reparsed = ParseNative(codec, *serialized.value);
    Check(reparsed != nullptr, "AddStringMod: reparse");
    if (!reparsed) return;

    const ObjectRecord* obj = reparsed->FindObject(ObjectKey{type, "H001"});
    Check(obj != nullptr, "AddStringMod: object found");
    if (!obj) return;
    auto fieldIt = std::find_if(
        obj->modifications.begin(), obj->modifications.end(),
        [](const ObjectModification& m) { return m.key.fieldId == "utip"; });
    Check(fieldIt != obj->modifications.end(), "AddStringMod: field found");
    if (fieldIt != obj->modifications.end()) {
        const auto* val = std::get_if<std::string>(&fieldIt->value);
        Check(val != nullptr && *val == "Custom Tooltip \xe4\xb8\xad\xe6\x96\x87",
              "AddStringMod: UTF-8 string value preserved");
    }
}

// ============================================================================
// 测试 6：Native AddModification Real/Unreal 类型往返
// ============================================================================
void TestNativeAddFloatModificationRoundtrip() {
    const ObjectType type = ObjectType::Unit;
    const std::vector<std::byte> fixture = BuildMinimalFixture(type);
    NativeObjectCodec codec(type);
    auto document = ParseNative(codec, fixture);
    Check(document != nullptr, "AddFloatMod: parse");
    if (!document) return;

    ModificationKey realField;
    realField.fieldId = "umvs";
    Status addReal = document->AddModification(
        ObjectKey{type, "H001"},
        realField,
        NativeValueType::Real,
        3.14f);
    Check(addReal.Succeeded(), "AddFloatMod: add real field");

    ModificationKey unrealField;
    unrealField.fieldId = "usca";
    Status addUnreal = document->AddModification(
        ObjectKey{type, "H001"},
        unrealField,
        NativeValueType::Unreal,
        1.5f);
    Check(addUnreal.Succeeded(), "AddFloatMod: add unreal field");

    auto serialized = codec.Serialize(*document);
    Check(serialized.Succeeded(), "AddFloatMod: serialize");
    if (!serialized.value.has_value()) return;

    auto reparsed = ParseNative(codec, *serialized.value);
    Check(reparsed != nullptr, "AddFloatMod: reparse");
    if (!reparsed) return;

    const ObjectRecord* obj = reparsed->FindObject(ObjectKey{type, "H001"});
    Check(obj != nullptr, "AddFloatMod: object found");
    if (!obj) return;

    auto realIt = std::find_if(
        obj->modifications.begin(), obj->modifications.end(),
        [](const ObjectModification& m) { return m.key.fieldId == "umvs"; });
    Check(realIt != obj->modifications.end(), "AddFloatMod: real field found");
    if (realIt != obj->modifications.end()) {
        const auto* val = std::get_if<float>(&realIt->value);
        Check(val != nullptr && *val == 3.14f, "AddFloatMod: real value");
    }

    auto unrealIt = std::find_if(
        obj->modifications.begin(), obj->modifications.end(),
        [](const ObjectModification& m) { return m.key.fieldId == "usca"; });
    Check(unrealIt != obj->modifications.end(), "AddFloatMod: unreal field found");
    if (unrealIt != obj->modifications.end()) {
        const auto* val = std::get_if<float>(&unrealIt->value);
        Check(val != nullptr && *val == 1.5f, "AddFloatMod: unreal value");
    }
}

// ============================================================================
// 测试 7：LNI AddObject 往返
// ============================================================================
void TestLniAddObjectRoundtrip() {
    const std::string_view lniSource =
        "[hfoo]\n"
        "_parent = \"hfoo\"\n"
        "unam = 100\n"
        "uhpm = 500\n";

    LniObjectCodec codec(ObjectType::Unit);
    auto document = ParseLni(codec, lniSource);
    Check(document != nullptr, "LNI AddObject: parse");
    if (!document) return;
    Check(document->IsStructurallySafe(), "LNI AddObject: safe");
    Check(document->Objects().size() == 1, "LNI AddObject: initial count");

    // 新增对象
    Status addStatus = document->AddObject("X001", "hfoo");
    Check(addStatus.Succeeded(), "LNI AddObject: add");
    Check(document->Objects().size() == 2, "LNI AddObject: count after add");

    // 不允许重复添加
    Status dupStatus = document->AddObject("X001", "hfoo");
    Check(!dupStatus.Succeeded(), "LNI AddObject: duplicate rejected");

    // 向新对象追加字段（通过 SetModification）
    ModificationKey field;
    field.fieldId = "unam";
    Status modStatus = document->SetModification(
        ObjectKey{ObjectType::Unit, "X001"},
        field,
        std::int32_t{250});
    Check(modStatus.Succeeded(), "LNI AddObject: set modification on new object");

    // 序列化
    auto serialized = codec.Serialize(*document);
    Check(serialized.Succeeded(), "LNI AddObject: serialize");
    if (!serialized.value.has_value()) return;

    // 重新解析
    auto reparsed = ParseLni(codec, std::string_view(
        reinterpret_cast<const char*>(serialized.value->data()),
        serialized.value->size()));
    Check(reparsed != nullptr, "LNI AddObject: reparse");
    if (!reparsed) return;
    Check(reparsed->IsStructurallySafe(), "LNI AddObject: reparsed safe");
    Check(reparsed->Objects().size() == 2, "LNI AddObject: reparsed count");

    // 验证新对象
    const ObjectRecord* newObj = reparsed->FindObject(ObjectKey{ObjectType::Unit, "X001"});
    Check(newObj != nullptr, "LNI AddObject: new object found");
    if (newObj) {
        Check(newObj->parentRawcode == "hfoo", "LNI AddObject: parent preserved");
        Check(newObj->modifications.size() == 1, "LNI AddObject: new object mod count");
        if (!newObj->modifications.empty()) {
            const auto* val = std::get_if<std::int32_t>(&newObj->modifications[0].value);
            Check(val != nullptr && *val == 250, "LNI AddObject: new object field value");
        }
    }

    // 验证原有对象未受影响
    const ObjectRecord* origObj = reparsed->FindObject(ObjectKey{ObjectType::Unit, "hfoo"});
    Check(origObj != nullptr, "LNI AddObject: original object preserved");
    if (origObj) {
        Check(origObj->modifications.size() == 2, "LNI AddObject: original mod count");
    }
}

// ============================================================================
// 测试 8：LNI AddObject + 多字段 往返
// ============================================================================
void TestLniAddObjectMultipleFieldsRoundtrip() {
    const std::string_view lniSource =
        "[hpea]\n"
        "_parent = \"hpea\"\n"
        "unam = 10\n";

    LniObjectCodec codec(ObjectType::Unit);
    auto document = ParseLni(codec, lniSource);
    Check(document != nullptr, "LNI MultiField: parse");
    if (!document) return;

    Status addStatus = document->AddObject("N002", "hpea");
    Check(addStatus.Succeeded(), "LNI MultiField: add object");

    // 追加多个字段
    ModificationKey f1;
    f1.fieldId = "uhpm";
    Check(document->SetModification(
        ObjectKey{ObjectType::Unit, "N002"}, f1, std::int32_t{1000}).Succeeded(),
        "LNI MultiField: add field 1");

    ModificationKey f2;
    f2.fieldId = "unam";
    Check(document->SetModification(
        ObjectKey{ObjectType::Unit, "N002"}, f2, std::string{"Hero"}).Succeeded(),
        "LNI MultiField: add field 2");

    ModificationKey f3;
    f3.fieldId = "umvs";
    Check(document->SetModification(
        ObjectKey{ObjectType::Unit, "N002"}, f3, 2.5f).Succeeded(),
        "LNI MultiField: add field 3");

    // 序列化并重新解析
    auto serialized = codec.Serialize(*document);
    Check(serialized.Succeeded(), "LNI MultiField: serialize");
    if (!serialized.value.has_value()) return;

    auto reparsed = ParseLni(codec, std::string_view(
        reinterpret_cast<const char*>(serialized.value->data()),
        serialized.value->size()));
    Check(reparsed != nullptr, "LNI MultiField: reparse");
    if (!reparsed) return;

    const ObjectRecord* obj = reparsed->FindObject(ObjectKey{ObjectType::Unit, "N002"});
    Check(obj != nullptr, "LNI MultiField: object found");
    if (!obj) return;
    Check(obj->modifications.size() == 3, "LNI MultiField: mod count");

    // 验证各字段
    for (const auto& mod : obj->modifications) {
        if (mod.key.fieldId == "uhpm") {
            const auto* val = std::get_if<std::int32_t>(&mod.value);
            Check(val != nullptr && *val == 1000, "LNI MultiField: uhpm value");
        } else if (mod.key.fieldId == "unam") {
            const auto* val = std::get_if<std::string>(&mod.value);
            Check(val != nullptr && *val == "Hero", "LNI MultiField: unam value");
        } else if (mod.key.fieldId == "umvs") {
            const auto* val = std::get_if<float>(&mod.value);
            Check(val != nullptr && *val == 2.5f, "LNI MultiField: umvs value");
        }
    }
}

// ============================================================================
// 测试 9：Native 错误路径验证
// ============================================================================
void TestNativeErrorPaths() {
    const ObjectType type = ObjectType::Unit;
    const std::vector<std::byte> fixture = BuildMinimalFixture(type);
    NativeObjectCodec codec(type);
    auto document = ParseNative(codec, fixture);
    Check(document != nullptr, "ErrorPaths: parse");
    if (!document) return;

    // 向不存在的对象追加字段
    ModificationKey field;
    field.fieldId = "xxxx";
    Status s1 = document->AddModification(
        ObjectKey{type, "ZZZZ"}, field, NativeValueType::Int32, std::int32_t{1});
    Check(!s1.Succeeded(), "ErrorPaths: add mod to nonexistent object");

    // 字段 ID 长度不为 4
    ModificationKey badField;
    badField.fieldId = "ab";
    Status s2 = document->AddModification(
        ObjectKey{type, "H001"}, badField, NativeValueType::Int32, std::int32_t{1});
    Check(!s2.Succeeded(), "ErrorPaths: invalid field id length");

    // 重复添加已存在的字段
    ModificationKey existField;
    existField.fieldId = "udef";
    Status s3 = document->AddModification(
        ObjectKey{type, "H001"}, existField, NativeValueType::Int32, std::int32_t{1});
    Check(!s3.Succeeded(), "ErrorPaths: duplicate field rejected");

    // 值类型不匹配
    ModificationKey newField;
    newField.fieldId = "udef";
    Status s4 = document->AddModification(
        ObjectKey{type, "H001"}, newField, NativeValueType::Int32, std::string{"wrong"});
    Check(!s4.Succeeded(), "ErrorPaths: value type mismatch");

    // 新增对象 rawcode 无效
    Status s5 = document->AddCustomObject("ab", "hfoo");
    Check(!s5.Succeeded(), "ErrorPaths: invalid rawcode length");

    // 新增对象重复
    Status s6 = document->AddCustomObject("H001", "hfoo");
    Check(!s6.Succeeded(), "ErrorPaths: duplicate object rejected");

    // 删除不存在的对象
    Status s7 = document->DeleteCustomObject(ObjectKey{type, "ZZZZ"});
    Check(!s7.Succeeded(), "ErrorPaths: delete nonexistent object");

    // 删除原始对象
    Status s8 = document->DeleteCustomObject(ObjectKey{type, "hfoo"});
    Check(!s8.Succeeded(), "ErrorPaths: delete original object rejected");
}

// ============================================================================
// 测试 10：Native 零修改往返（所有七种类型）
// ============================================================================
void TestNativeZeroEditRoundtrip() {
    constexpr std::array types{
        ObjectType::Unit,
        ObjectType::Ability,
        ObjectType::Item,
        ObjectType::Buff,
        ObjectType::Upgrade,
        ObjectType::Doodad,
        ObjectType::Destructable,
    };

    for (const ObjectType type : types) {
        const std::vector<std::byte> fixture = BuildMinimalFixture(type);
        NativeObjectCodec codec(type);
        auto document = ParseNative(codec, fixture);
        Check(document != nullptr, "ZeroEdit: parse");
        if (!document) continue;

        // 不做任何修改直接序列化
        auto serialized = codec.Serialize(*document);
        Check(serialized.Succeeded(), "ZeroEdit: serialize");
        if (serialized.value.has_value()) {
            Check(*serialized.value == fixture, "ZeroEdit: byte-identical roundtrip");
        }
    }
}

} // namespace

int main() {
    TestNativeAddModificationRoundtrip();
    TestNativeAddCustomObjectRoundtrip();
    TestNativeDeleteCustomObjectRoundtrip();
    TestNativeCombinedOperationsRoundtrip();
    TestNativeAddStringModificationRoundtrip();
    TestNativeAddFloatModificationRoundtrip();
    TestLniAddObjectRoundtrip();
    TestLniAddObjectMultipleFieldsRoundtrip();
    TestNativeErrorPaths();
    TestNativeZeroEditRoundtrip();

    if (g_failures != 0) {
        std::cerr << g_failures << " compat roundtrip test(s) failed.\n";
        return EXIT_FAILURE;
    }
    std::cout << "All compat roundtrip tests passed.\n";
    return EXIT_SUCCESS;
}
