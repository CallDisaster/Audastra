#include "ObjectCommandsFixture.h"
#include "War3MapCore/Services/ObjectFieldText.h"

#include <cmath>
#include <iostream>
#include <limits>

namespace {
using namespace War3Map;
using namespace ObjectData;
using namespace Services;
using Tests::ObjectCommandsFixture;
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void AllKindsReadCreateAndRoundtrip() {
    ObjectCommandsFixture fixture;
    for (const auto& kind : ObjectCommandsFixture::kinds) {
        auto context = fixture.Context(kind.type);
        const auto rows = ListObjects(context, ObjectListScope::All);
        Require(rows.Succeeded() && rows.value->size() == 1 && rows.value->front().source == "standard", "seven kinds standard list");
        const auto fields = ReadObjectFields(context, kind.base);
        Require(fields.Succeeded() && !fields.value->empty(), "seven kinds metadata fields");
        const auto created = PlanObjectCreation(context, kind.base, std::string(kind.custom), std::string("副本名称"));
        Require(created.Succeeded(), "seven kinds create and name");
        const auto* copy = created.value->after.FindObject({kind.type, kind.custom});
        Require(copy && copy->parentRawcode == kind.base && copy->stableId == MakeObjectStableId(copy->key), "creation stable typed identity");
        const auto encoded = LniObjectCodec(kind.type).Serialize(created.value->after);
        Require(encoded.Succeeded(), "creation serialized");
        const auto decoded = LniObjectCodec(kind.type).Parse(*encoded.value);
        const auto* document = decoded.Succeeded() ? dynamic_cast<const LniObjectDocument*>(decoded.document.get()) : nullptr;
        Require(document && DiffObjects(created.value->after.Objects(), document->Objects()).Empty(), "creation semantic roundtrip");
        *fixture.documents[static_cast<std::size_t>(kind.type)] = created.value->after;
        context = fixture.Context(kind.type);
        Require(!PlanObjectCreation(context, kind.base, std::string(kind.custom), {}).Succeeded(), "id collision rejected");
        const auto automatic = PlanObjectCreation(context, kind.custom, {}, {});
        Require(automatic.Succeeded() && automatic.value->subject.rawcode != kind.custom, "clone custom with generated id");
        const auto custom = ListObjects(context, ObjectListScope::Custom);
        Require(custom.Succeeded() && custom.value->size() == 1 && custom.value->front().name == "副本名称", "custom list name from actual field");
    }
}
void AtomicFieldTypesAndReferences() {
    ObjectCommandsFixture fixture;
    const auto context = fixture.Context(ObjectType::Unit);
    const auto original = context.document->DocumentFingerprint();
    const std::array valid{ObjectFieldChange{"uhpm", {}, {}, std::int32_t{250}},
        ObjectFieldChange{"uabi", {}, {}, RawcodeListValue{{"A001", "A001"}}},
        ObjectFieldChange{"uspe", {}, {}, true}, ObjectFieldChange{"urac", {}, {}, std::string("orc")}};
    const auto edited = PlanObjectFieldChanges(context, "u001", valid);
    Require(edited.Succeeded() && context.document->DocumentFingerprint() == original, "plan does not publish mutation");
    const auto* record = edited.value->after.FindObject({ObjectType::Unit, "u001"});
    Require(record && record->modifications.size() == 4, "valid batch changed all fields");
    for (const auto& field : record->modifications) if (field.key.fieldId == "uabi")
        Require(std::get<std::string>(field.value) == "A001,A001", "list preserves order and duplicate instances");
    for (const auto& invalid : std::vector<ObjectFieldChange>{
        {"uhpm", {}, {}, std::string("250")}, {"uhpm", {}, {}, std::int32_t{0}},
        {"ussc", {}, {}, (std::numeric_limits<float>::infinity)()}, {"urac", {}, {}, std::string("invalid")},
        {"uabi", {}, {}, std::string("A001")}, {"uabi", {}, {}, RawcodeListValue{{"A999"}}},
        {"uspe", {}, {}, std::int32_t{1}}, {"uhpm", 1, {}, std::int32_t{250}},
        {"uhpm", {}, 1, std::int32_t{250}}}) {
        const std::array batch{valid.front(), invalid};
        Require(!PlanObjectFieldChanges(context, "u001", batch).Succeeded(), "bad trailing field rejects whole batch");
        Require(context.document->DocumentFingerprint() == original, "rejected batch preserves source");
    }
    const std::array duplicate{valid.front(), ObjectFieldChange{"HP", {}, {}, std::int32_t{300}}};
    Require(!PlanObjectFieldChanges(context, "u001", duplicate).Succeeded(), "canonical duplicate aliases reject");
    const auto ability = fixture.Context(ObjectType::Ability);
    const std::array level{ObjectFieldChange{"adat", 2, {}, std::int32_t{50}}};
    const auto changedLevel = PlanObjectFieldChanges(ability, "A001", level);
    Require(changedLevel.Succeeded(), "ability native level supported");
    const auto* changedAbility = changedLevel.value->after.FindObject({ObjectType::Ability, "A001"});
    Require(changedAbility && changedAbility->modifications[0].key.level == 2u && changedAbility->modifications[0].key.dataIndex == 1u, "exact native level/data index");
    Require(!ResolveObjectField(ability, "A001", "adat", 0).Succeeded(), "level zero never silently becomes first level");
    Require(!ResolveObjectField(ability, "A001", "dataa1", 2).Succeeded(), "source key level mismatch rejects");
}
void CompactTextRoundtrip() {
    ObjectCommandsFixture fixture;
    auto context = fixture.Context(ObjectType::Unit);
    const std::array edits{ObjectFieldChange{"uhpm", {}, {}, std::int32_t{250}},
        ObjectFieldChange{"uabi", {}, {}, RawcodeListValue{{"A001", "A001"}}}, ObjectFieldChange{"uspe", {}, {}, true}};
    const auto edited = PlanObjectFieldChanges(context, "u001", edits);
    Require(edited.Succeeded(), "text fixture edit");
    *fixture.documents[0] = edited.value->after; context = fixture.Context(ObjectType::Unit);
    const auto fields = ReadObjectFields(context, "u001");
    Require(fields.Succeeded(), "text fixture fields");
    const auto text = RenderObjectFieldText(*fields.value);
    Require(text.Succeeded(), "render overridden fields");
    Require(text.value->find("(uhpm) = 250\n") != std::string::npos, "integer line");
    Require(text.value->find("(uabi) = ['A001', 'A001']") != std::string::npos, "ordered list keeps duplicates");
    Require(text.value->find("(uspe) = true") != std::string::npos, "boolean line");
    ObjectFieldTextOptions all; all.all = true;
    const auto full = RenderObjectFieldText(*fields.value, all);
    Require(full.Succeeded() && full.value->size() > text.value->size(), "all lists inherited fields");
    // 原样写回：解析成功且不产生任何变化。
    const auto parsed = ParseObjectFieldText(*text.value);
    Require(parsed.Succeeded() && parsed.value->size() == 3, "rendered text parses back");
    std::vector<ObjectFieldChange> same;
    for (const auto& row : *parsed.value) same.push_back(row.change);
    const auto unchanged = PlanObjectFieldChanges(context, "u001", same);
    Require(unchanged.Succeeded() && unchanged.value->changes.Empty(), "roundtrip is a no-op");
    // 只写要改的行；显示名、注释、空行和 # 行都可省略或保留。
    const auto edit = ParseObjectFieldText("# 改血量\n\n生命值 (uhpm) = 300  // 旧 250\nuspe = false\r\n");
    Require(edit.Succeeded() && edit.value->size() == 2 && edit.value->at(0).line == 3 && edit.value->at(1).line == 4, "edit lines");
    Require(std::get<std::int32_t>(edit.value->at(0).change.value) == 300 && !std::get<bool>(edit.value->at(1).change.value), "edit values");
    const auto leveled = ParseObjectFieldText("伤害 (adat:2) = 50\n模型 (umdl:1:3) = \"a\\\\b \\\"c\\\"\"\n缩放 = 1.5\n列表 = []");
    Require(leveled.Succeeded() && leveled.value->at(0).change.level == 2u && leveled.value->at(1).change.variation == 3u
        && std::get<std::string>(leveled.value->at(1).change.value) == "a\\b \"c\"" && std::get<float>(leveled.value->at(2).change.value) == 1.5f
        && std::get<RawcodeListValue>(leveled.value->at(3).change.value).values.empty(), "levels, variations, escapes, decimals");
    const auto bad = ParseObjectFieldText("(uhpm) = 1\n(uhpm:0) = 1\n(uspe) = yes\n(umdl) = \"open\nnothing here");
    Require(!bad.Succeeded() && bad.diagnostics.size() == 4, "every bad line reported");
    Require(bad.diagnostics[0].message.starts_with("第 2 行") && bad.diagnostics[3].message.starts_with("第 5 行"), "line numbers");
    Require(!ParseObjectFieldText("# only comment\n").Succeeded(), "empty text rejected");
}
void DeletionRemainsFailClosed() {
    ObjectCommandsFixture fixture;
    auto context = fixture.Context(ObjectType::Unit);
    const auto created = PlanObjectCreation(context, "u001", std::string("U001"), {});
    Require(created.Succeeded(), "delete fixture creation");
    *fixture.documents[0] = created.value->after; context = fixture.Context(ObjectType::Unit);
    ObjectDeletionAssessment assessment; assessment.subject = {ObjectType::Unit, "U001"};
    for (const auto disposition : {ObjectDeletionDisposition::BlockedReferences, ObjectDeletionDisposition::BlockedUnknown, ObjectDeletionDisposition::Eligible}) {
        assessment.disposition = disposition;
        Require(!PlanObjectDeletion(context, "U001", assessment).Succeeded(), "caller disposition cannot forge complete deletion proof");
    }
    Require(context.document->FindObject(assessment.subject), "rejected deletion preserved exact object");
}
}
int main() {
    try { AllKindsReadCreateAndRoundtrip(); AtomicFieldTypesAndReferences(); CompactTextRoundtrip(); DeletionRemainsFailClosed();
        std::cout << "ObjectCommands tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << "ObjectCommands test failure: " << error.what() << '\n'; return 1; }
}
