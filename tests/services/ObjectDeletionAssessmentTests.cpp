#include "War3MapCore/Services/ObjectDeletionAssessment.h"

#include "War3MapCore/MapDocument.h"
#include "War3MapCore/Trigger/TriggerDocument.h"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace War3Map;
using namespace War3Map::ObjectData;
using namespace War3Map::Services;

constexpr std::array<ObjectType, 7> kTypes{
    ObjectType::Unit, ObjectType::Ability, ObjectType::Item, ObjectType::Buff,
    ObjectType::Upgrade, ObjectType::Doodad, ObjectType::Destructable,
};

void Expect(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

bool HasReason(const ObjectDeletionAssessment& assessment, const std::string& code) {
    for (const auto& reason : assessment.reasons) {
        if (reason.code == code) return true;
    }
    return false;
}

std::vector<ObjectRecordSource> Sources(
    const std::vector<ObjectRecord>& units,
    const std::vector<ObjectRecord>& abilities = {}) {
    std::vector<ObjectRecordSource> result;
    for (const auto type : kTypes) {
        result.push_back({type, std::string(NativeFileName(type)), ReferenceInputState::Ready, {}});
        if (type == ObjectType::Unit) result.back().records = units;
        if (type == ObjectType::Ability) result.back().records = abilities;
    }
    return result;
}

ObjectRecord Custom(std::string rawcode, std::string parent = "hfoo") {
    ObjectRecord record;
    record.key = {ObjectType::Unit, std::move(rawcode)};
    record.parentRawcode = std::move(parent);
    return record;
}

void TestKnownParentReferenceBlocks() {
    const std::vector<ObjectRecord> units{Custom("H000"), Custom("H001", "H000")};
    const auto sources = Sources(units);
    const MapDocument map = MapDocument::FromMembers({});
    ProjectReferenceInputs inputs;
    inputs.objects = sources;
    inputs.map = &map;

    const auto assessment = AssessObjectDeletion(inputs, {ObjectType::Unit, "H000"});
    Expect(assessment.disposition == ObjectDeletionDisposition::BlockedReferences,
        "known typed parent reference must block deletion");
    Expect(assessment.objectReferences.definite.size() == 1,
        "typed parent reference must be reported");
    Expect(HasReason(assessment, "definite-object-reference"),
        "known reference must identify its source");
}

void TestZeroStaticMatchesNeverAuthorizesDeletion() {
    const std::vector<ObjectRecord> units{Custom("H000")};
    const auto sources = Sources(units);
    const MapDocument map = MapDocument::FromMembers({});
    ProjectReferenceInputs inputs;
    inputs.objects = sources;
    inputs.map = &map;

    const auto assessment = AssessObjectDeletion(inputs, {ObjectType::Unit, "H000"});
    Expect(assessment.objectReferences.definite.empty() && assessment.objectReferences.possible.empty(),
        "fixture has no visible static references");
    Expect(assessment.disposition == ObjectDeletionDisposition::BlockedUnknown,
        "static zero matches must not imply deletion approval");
    Expect(HasReason(assessment, "triggers-unavailable"), "missing trigger input must be explicit");
    Expect(HasReason(assessment, "source-incomplete"), "missing placement members must be explicit");
    Expect(HasReason(assessment, "script-source-unverified"), "unknown JASS source must be explicit");
}

void TestPossibleReferenceAndIncompleteObjectField() {
    const std::vector<ObjectRecord> units{Custom("H000")};
    ObjectRecord ability;
    ability.key = {ObjectType::Ability, "A000"};
    ObjectModification field;
    field.key.fieldId = "test";
    field.value = RawcodeValue{"H000"};
    ability.modifications.push_back(field);
    const std::vector<ObjectRecord> abilities{ability};
    const auto sources = Sources(units, abilities);
    const MapDocument map = MapDocument::FromMembers({});
    ProjectReferenceInputs inputs;
    inputs.objects = sources;
    inputs.map = &map;

    const auto assessment = AssessObjectDeletion(inputs, {ObjectType::Unit, "H000"});
    Expect(assessment.disposition == ObjectDeletionDisposition::BlockedUnknown,
        "untyped rawcode field must remain uncertain");
    Expect(assessment.objectReferences.possible.size() == 1,
        "possible typed reference must be retained");
    Expect(HasReason(assessment, "possible-object-reference"),
        "possible reference reason must be visible");
}

void TestCustomTextDynamicValueAndGuiToken() {
    const std::vector<ObjectRecord> units{Custom("H000")};
    const auto sources = Sources(units);
    const MapDocument map = MapDocument::FromMembers({});
    Trigger::TriggerDocument triggers;
    triggers.wct.customCode.text = "function Custom takes nothing returns nothing\nset id = 'H000'";
    Trigger::ArgumentNode argument;
    argument.type = 1;
    argument.value.text = "H000";
    triggers.argumentNodes.push_back(argument);
    Trigger::EcaNode action;
    action.name.text = "CreateUnit";
    action.arguments.push_back(0);
    triggers.ecaNodes.push_back(action);
    Trigger::TriggerRecord trigger;
    trigger.name.text = "Custom";
    trigger.customTextFlag = 1;
    trigger.customText.text = "call CreateUnit(...)";
    trigger.ecas.push_back(0);
    triggers.triggers.push_back(trigger);

    ProjectReferenceInputs inputs;
    inputs.objects = sources;
    inputs.map = &map;
    inputs.triggers = &triggers;
    const auto assessment = AssessObjectDeletion(inputs, {ObjectType::Unit, "H000"});
    Expect(assessment.disposition == ObjectDeletionDisposition::BlockedUnknown,
        "custom code and dynamic GUI input must block eligibility");
    Expect(HasReason(assessment, "wct-custom-code"), "WCT custom code reason missing");
    Expect(HasReason(assessment, "trigger-custom-text"), "trigger custom text reason missing");
    Expect(HasReason(assessment, "dynamic-gui-value"), "dynamic GUI reason missing");
    Expect(HasReason(assessment, "untyped-gui-reference"), "untyped GUI match reason missing");
    Expect(assessment.untypedGuiReferences.size() == 1, "GUI reference location must be available");
    Expect(assessment.dynamicValueCount == 1 && assessment.dynamicValues.front().variable,
        "dynamic argument position must be available");
    Expect(assessment.scriptHintCount == 1 && assessment.scriptHints.front().line == 2,
        "WCT candidate rawcode line must be available without treating it as a proven reference");
}

void TestSuppliedScriptBytesAreHintsNotAuthorization() {
    const std::vector<ObjectRecord> units{Custom("H000")};
    const auto sources = Sources(units);
    const MapDocument map = MapDocument::FromMembers({});
    ProjectReferenceInputs inputs;
    inputs.objects = sources;
    inputs.map = &map;

    const std::string script = "// H000 in a comment\ncall SetType('H000')\n";
    const std::array<ScriptSourceInput, 1> scripts{{{"war3map.j",
        {reinterpret_cast<const std::byte*>(script.data()), script.size()}}}};
    auto assessment = AssessObjectDeletion(inputs, {ObjectType::Unit, "H000"}, scripts);
    Expect(assessment.disposition == ObjectDeletionDisposition::BlockedUnknown,
        "supplying actual script bytes must not claim complete semantic coverage");
    Expect(assessment.scriptHintCount == 2 && assessment.scriptHints.size() == 2,
        "all rawcode text candidates must be counted");
    Expect(assessment.scriptHints[0].line == 1 && assessment.scriptHints[1].line == 2,
        "candidate source line must be correct");
    Expect(HasReason(assessment, "possible-script-reference"),
        "script candidate reason must distinguish hints from proven references");
    Expect(HasReason(assessment, "script-source-unverified"),
        "provided scripts do not prove inventory or dynamic values complete");

    const std::string unrelated = "call DoNothing()";
    const std::array<ScriptSourceInput, 1> noMatch{{{"war3map.j",
        {reinterpret_cast<const std::byte*>(unrelated.data()), unrelated.size()}}}};
    assessment = AssessObjectDeletion(inputs, {ObjectType::Unit, "H000"}, noMatch);
    Expect(assessment.scriptHintCount == 0 && assessment.disposition == ObjectDeletionDisposition::BlockedUnknown,
        "zero rawcode text matches still must not authorize deletion");
}

void TestInvalidSourcesAndOriginalObject() {
    const std::vector<ObjectRecord> units{Custom("H000")};
    auto sources = Sources(units);
    sources.front().state = ReferenceInputState::Invalid;
    const MapDocument map = MapDocument::FromMembers({});
    ProjectReferenceInputs inputs;
    inputs.objects = sources;
    inputs.map = &map;
    auto assessment = AssessObjectDeletion(inputs, {ObjectType::Unit, "H000"});
    Expect(assessment.disposition == ObjectDeletionDisposition::BlockedUnknown,
        "invalid object source must block eligibility");
    Expect(HasReason(assessment, "source-incomplete"), "invalid source reason missing");

    ObjectRecord original;
    original.key = {ObjectType::Unit, "hfoo"};
    const std::vector<ObjectRecord> originalUnits{original};
    sources = Sources(originalUnits);
    inputs.objects = sources;
    assessment = AssessObjectDeletion(inputs, original.key);
    Expect(HasReason(assessment, "original-object"), "original object cannot be deleted");
    Expect(assessment.disposition == ObjectDeletionDisposition::BlockedUnknown,
        "original object must not become eligible");
}

} // namespace

int main() {
    try {
        TestKnownParentReferenceBlocks();
        TestZeroStaticMatchesNeverAuthorizesDeletion();
        TestPossibleReferenceAndIncompleteObjectField();
        TestCustomTextDynamicValueAndGuiToken();
        TestSuppliedScriptBytesAreHintsNotAuthorization();
        TestInvalidSourcesAndOriginalObject();
        std::cout << "ObjectDeletionAssessment tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ObjectDeletionAssessment test failure: " << error.what() << '\n';
        return 1;
    }
}
