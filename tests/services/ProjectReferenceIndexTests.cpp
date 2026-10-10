#include "War3MapCore/Services/ProjectReferenceIndex.h"
#include "War3MapCore/Trigger/TriggerDocument.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using namespace War3Map;
using namespace War3Map::Services;

void Expect(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::uint32_t AddArgument(Trigger::TriggerDocument& document, std::int32_t type, std::string value) {
    Trigger::ArgumentNode argument;
    argument.id = 1000 + document.argumentNodes.size();
    argument.type = type;
    argument.value.text = std::move(value);
    document.argumentNodes.push_back(std::move(argument));
    return static_cast<std::uint32_t>(document.argumentNodes.size() - 1);
}

std::uint32_t AddEca(Trigger::TriggerDocument& document, std::int32_t type, std::string name,
    std::vector<std::uint32_t> arguments, bool enabled = true) {
    Trigger::EcaNode eca;
    eca.id = 2000 + document.ecaNodes.size();
    eca.type = type;
    eca.name.text = std::move(name);
    eca.enabled = enabled ? 1 : 0;
    eca.arguments = std::move(arguments);
    document.ecaNodes.push_back(std::move(eca));
    return static_cast<std::uint32_t>(document.ecaNodes.size() - 1);
}

// 原创合成工程：两个触发器、一个带初值的变量、两个物编对象。
Trigger::TriggerDocument MakeTriggers() {
    Trigger::TriggerDocument document;
    Trigger::VariableRecord wave;
    wave.name.text = "Wave";
    wave.hasDefault = 1;
    wave.value.text = "hfoo";
    document.variables.push_back(wave);

    // 动作 CreateUnit(玩家预设, 'hfoo', GetRandomLoc(变量 udg_SpawnArea))
    const auto nestedArgument = AddArgument(document, 1, "udg_SpawnArea");
    const auto call = AddEca(document, 3, "GetRandomLoc", {nestedArgument});
    auto callArgument = AddArgument(document, 2, "GetRandomLoc");
    document.argumentNodes[callArgument].insertCall = 1;
    document.argumentNodes[callArgument].callEca = call;
    const auto create = AddEca(document, 2, "CreateUnit",
        {AddArgument(document, 0, "Player00"), AddArgument(document, 3, "hfoo"), callArgument});
    const auto event = AddEca(document, 0, "MapInitializationEvent", {});
    // 禁用的语句：引用仍列出，但标为禁用。
    const auto disabled = AddEca(document, 2, "RemoveUnit", {AddArgument(document, 3, "Hpal")}, false);

    Trigger::TriggerRecord setup;
    setup.id = 1;
    setup.name.text = "Setup";
    setup.ecas = {event, create, disabled};
    document.triggers.push_back(setup);

    // 已禁用的触发器：其中所有引用都视为禁用。
    const auto other = AddEca(document, 2, "CreateUnit", {AddArgument(document, 3, "hfoo")});
    Trigger::TriggerRecord off;
    off.id = 2;
    off.name.text = "Off";
    off.enabled = 0;
    off.ecas = {other};
    document.triggers.push_back(off);
    return document;
}

void TestTriggerAndObjectReferences() {
    const auto triggers = MakeTriggers();
    ObjectData::ObjectRecord custom;
    custom.key = {ObjectData::ObjectType::Unit, "h000"};
    custom.parentRawcode = "hfoo";
    ObjectData::ObjectModification upgrade;
    upgrade.key.fieldId = "upgs";
    upgrade.value = ObjectData::RawcodeListValue{{"Rhme", "Rhar"}};
    custom.modifications.push_back(upgrade);
    const std::vector<ObjectData::ObjectRecord> units{custom};
    const std::vector<ObjectData::ObjectRecordSource> sources{
        {ObjectData::ObjectType::Unit, "war3map.w3u", ObjectData::ReferenceInputState::Ready, units}};

    ProjectReferenceInputs inputs;
    inputs.triggers = &triggers;
    inputs.objects = sources;
    const auto index = ProjectReferenceIndex::Build(inputs);

    const std::vector<std::string> footman{"hfoo"};
    const auto usages = index.UsagesOf(footman);
    // Setup 的 CreateUnit、Off 的 CreateUnit、变量 Wave 初值、h000 的父对象。
    Expect(usages.size() == 4, "hfoo usages: expected 4, got " + std::to_string(usages.size()));
    Expect(usages[0]->module == UsageModule::Triggers && usages[0]->owner == "Setup" &&
        usages[0]->place == UsagePlace::Action && !usages[0]->disabled, "active trigger statement");
    Expect(usages[1]->owner == "Off" && usages[1]->disabled, "statement in disabled trigger must be marked");
    Expect(usages[2]->module == UsageModule::Variables && usages[2]->owner == "Wave", "variable initial value");
    Expect(usages[3]->module == UsageModule::Objects && usages[3]->place == UsagePlace::ObjectParent &&
        usages[3]->ownerObject && usages[3]->ownerObject->rawcode == "h000", "object parent reference");

    // 物体自身的字段不算对自己的引用。
    const std::vector<std::string> self{"h000"};
    Expect(index.UsagesOf(self).empty(), "unreferenced custom object");
    const std::vector<std::string> armor{"Rhar"};
    const auto armorUses = index.UsagesOf(armor);
    Expect(armorUses.size() == 1 && armorUses[0]->detail == "upgs", "list field token with field id");
    Expect(index.UsagesOf(armor, ObjectOwnerId({ObjectData::ObjectType::Unit, "h000"})).empty(), "exclude owner");

    // 嵌套调用中的变量归到外层语句；udg_ 前缀同时登记裸名。
    const std::vector<std::string> area{"SpawnArea"};
    const auto areaUses = index.UsagesOf(area);
    Expect(areaUses.size() == 1 && areaUses[0]->ecaIndex && triggers.ecaNodes[*areaUses[0]->ecaIndex].name.text == "CreateUnit",
        "nested call argument belongs to the enclosing statement");

    const std::vector<std::string> paladin{"Hpal"};
    const auto paladinUses = index.UsagesOf(paladin);
    Expect(paladinUses.size() == 1 && paladinUses[0]->disabled, "disabled statement keeps its reference");

    Expect(index.CallsOf("CreateUnit").size() == 2, "function calls");
    Expect(index.CallsOf("GetRandomLoc").size() == 1, "nested function call");
    Expect(index.CallsOf("Missing").empty(), "unknown function");
    const auto functions = index.CalledFunctions();
    Expect(functions.size() == 4, "called function catalog");

    const auto& outgoing = index.Outgoing(TriggerOwnerId(1));
    Expect(outgoing.contains("hfoo") && outgoing.contains("Player00") && outgoing.contains("SpawnArea"), "outgoing tokens");
    Expect(index.Outgoing("trigger:999").empty(), "unknown owner");
    Expect(!index.Coverage().empty(), "coverage is reported");
}

} // namespace

int main() {
    try {
        TestTriggerAndObjectReferences();
        std::cout << "ProjectReferenceIndex tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ProjectReferenceIndex test failure: " << error.what() << '\n';
        return 1;
    }
}
