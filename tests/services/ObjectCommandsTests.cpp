#include "ObjectCommandsFixture.h"

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
    try { AllKindsReadCreateAndRoundtrip(); AtomicFieldTypesAndReferences(); DeletionRemainsFailClosed();
        std::cout << "ObjectCommands tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << "ObjectCommands test failure: " << error.what() << '\n'; return 1; }
}
