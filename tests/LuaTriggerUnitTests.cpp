#include "War3MapCore/Script/LuaTriggerUnit.h"
#include "../src/ScriptBuild/ExternalProcess.h"
#include "TestLua.h"

#include <Windows.h>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>

using namespace War3Map;
using namespace War3Map::Script;
void TestLuaTriggerCandidate(const Trigger::UiCatalog&, const Script::LuaTargetProfile&);
namespace {
void Require(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
void Write(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc); stream << text;
    Require(static_cast<bool>(stream), "write failed");
}
struct Fixture {
    Trigger::TriggerDocument doc;
    Trigger::UiCatalog catalog;
    LuaTargetProfile profile;
    std::vector<LuaCallbackBinding> callbacks{{"ResearchTimerDone", 119}, {"ResearchStart", 120}};
    std::map<std::string, int> kinds;
    StableId next = 1000;
    std::string event, condition, action, call;
    void Definition(int kind, std::string name, std::string result, std::vector<std::string> args,
        std::string script = {}) {
        kinds[name] = kind;
        auto& text = kind == 0 ? event : kind == 1 ? condition : kind == 2 ? action : call;
        text += "[" + name + "]\n";
        if (!result.empty()) text += "returns=" + result + "\n";
        if (!script.empty()) text += "script_name=" + script + "\n";
        for (const auto& type : args) text += "[[.args]]\ntype=" + type + "\n";
    }
    void Api(std::string name, std::string result, std::vector<std::string> parameters) {
        Definition(result == "nothing" ? 2 : 3, name, result, parameters);
        profile.apis.push_back({std::move(name), std::move(result), std::move(parameters)});
    }
    explicit Fixture(const std::filesystem::path& root) {
        profile.identity = "original-test-stubs/lua53/classic-int32";
        for (const char* name : {"unit", "item", "group", "player", "timer", "trigger", "event", "boolexpr", "triggercondition", "triggeraction"})
            profile.handleTypes.emplace_back(name, "handle");
        Api("CreateTrigger", "trigger", {}); Api("Condition", "boolexpr", {"code"});
        Api("TriggerAddCondition", "triggercondition", {"trigger", "boolexpr"});
        Api("TriggerAddAction", "triggeraction", {"trigger", "code"});
        Api("DisableTrigger", "nothing", {"trigger"}); Api("ConditionalTriggerExecute", "nothing", {"trigger"});
        Api("TriggerRegisterTimerEvent", "event", {"trigger", "real", "boolean"});
        Api("I2R", "real", {"integer"}); Api("I2S", "string", {"integer"}); Api("FourCC", "integer", {"string"});
        Api("GetUnitTypeId", "integer", {"unit"}); Api("CreateGroup", "group", {});
        Api("Player", "player", {"integer"}); Api("CreateUnit", "unit", {"player", "unitcode", "real", "real", "real"});
        profile.apis.back().parameters[1] = "integer";
        Api("CreateItem", "item", {"itemcode", "real", "real"}); profile.apis.back().parameters[0] = "integer";
        Api("CreateTimer", "timer", {}); Api("TimerStart", "nothing", {"timer", "real", "boolean", "code"});
        Api("ExecuteFunc", "nothing", {"string"}); Api("GetHandleId", "integer", {"handle"});
        Api("GetRandomInt", "integer", {"integer", "integer"}); Api("GetRandomReal", "real", {"real", "real"});
        Api("ArrayIndexProbe", "integer", {}); Api("ArrayValueProbe", "integer", {});
        Api("StringProbe", "string", {"string"});
        Definition(2, "SetVariable", "", {"AnyGlobal", "AnyType"});
        Definition(2, "ForLoopVarMultiple", "", {"integer", "integer", "integer"});
        Definition(2, "IfThenElseMultiple", "", {}); Definition(2, "ReturnAction", "", {});
        Definition(3, "OperatorInt", "integer", {"integer", "ArithmeticOperator", "integer"});
        Definition(3, "OperatorReal", "real", {"real", "ArithmeticOperator", "real"});
        Definition(3, "OperatorString", "string", {"string", "string"});
        Definition(3, "GetBooleanAnd", "boolean", {"boolean", "boolean"});
        Definition(3, "GetBooleanOr", "boolean", {"boolean", "boolean"});
        Definition(3, "Null", "AnyReturnType", {});
        Definition(1, "GetBooleanVariableValue", "boolean", {"boolean"}); Definition(1, "AndMultiple", "boolean", {});
        Definition(1, "CompareInt", "boolean", {"integer", "ComparisonOperator", "integer"});
        Definition(1, "CompareUnit", "boolean", {"unit", "EqualNotEqualOperator", "unit"});
        Definition(0, "TimerEvent", "", {"real", "boolean"}, "TriggerRegisterTimerEvent");
        Definition(0, "MapInitializationEvent", "", {});
        Write(root / "ui/config", "base\n");
        Write(root / "ui/base/define.txt", "[TriggerTypes]\nunitcode=0,1,1,Unit,integer\nitemcode=0,1,1,Item,integer\n"
            "[TriggerTypeDefaults]\ngroup=CreateGroup()\n");
        Write(root / "ui/base/event.txt", event); Write(root / "ui/base/condition.txt", condition);
        Write(root / "ui/base/action.txt", action); Write(root / "ui/base/call.txt", call);
        auto loaded = Trigger::UiCatalog::Load(root); Require(loaded.Succeeded(), "catalog load"); catalog = std::move(*loaded.value);
        for (const auto& [name, type] : std::vector<std::pair<std::string, std::string>>{
            {"Count","integer"},{"Quotient","integer"},{"Value","real"},{"Label","string"},{"Flag","boolean"},
            {"U","unit"},{"V","unit"},{"Item","item"},{"T","timer"},{"I","integer"},{"Limit","integer"},
            {"HandleNumber","integer"},{"Roll","integer"},{"Chance","real"},{"Values","integer"},{"Checks","boolean"},{"Groups","group"},
            {"Labels","string"},{"Reals","real"}}) {
            Trigger::VariableRecord variable; variable.id = next++; variable.name.text = name; variable.typeName.text = type;
            variable.isArray = name == "Values" || name == "Checks" || name == "Groups" || name == "Labels" || name == "Reals";
            variable.size = 1; doc.variables.push_back(variable);
        }
        for (StableId i = 101; i <= 140; ++i) {
            Trigger::TriggerRecord trigger; trigger.id = i; trigger.name.text = "Research" + std::to_string(i);
            doc.triggers.push_back(trigger);
        }
    }
    std::uint32_t Value(std::string value, int type = 3, std::optional<std::uint32_t> index = {}) {
        Trigger::ArgumentNode argument; argument.id = next++; argument.type = type; argument.value.text = std::move(value);
        argument.indexArgument = index; argument.insertIndex = index ? 1 : 0;
        doc.argumentNodes.push_back(argument); return static_cast<std::uint32_t>(doc.argumentNodes.size() - 1);
    }
    std::uint32_t Var(std::string name, std::optional<std::uint32_t> index = {}) { return Value(std::move(name), 1, index); }
    std::uint32_t Eca(std::string name, std::vector<std::uint32_t> arguments) {
        Trigger::EcaNode node; node.id = next++; node.type = kinds.at(name); node.name.text = std::move(name); node.arguments = std::move(arguments);
        doc.ecaNodes.push_back(node); return static_cast<std::uint32_t>(doc.ecaNodes.size() - 1);
    }
    std::uint32_t Call(std::string name, std::vector<std::uint32_t> arguments) {
        const auto eca = Eca(std::move(name), std::move(arguments)); const auto index = Value("", 2);
        doc.argumentNodes[index].callEca = eca; doc.argumentNodes[index].insertCall = 1; return index;
    }
    std::uint32_t Int(std::uint32_t left, const char* op, std::uint32_t right) { return Call("OperatorInt", {left, Value(op), right}); }
    std::uint32_t Set(std::uint32_t target, std::uint32_t value) { return Eca("SetVariable", {target, value}); }
    void Root(unsigned test, std::uint32_t eca) { doc.triggers[test - 1].ecas.push_back(eca); }
    void SetRoot(unsigned test, std::string name, std::uint32_t value) { Root(test, Set(Var(std::move(name)), value)); }
    void Samples() {
        SetRoot(1, "Count", Int(Value("4"), "+", Value("5")));
        SetRoot(2, "Quotient", Int(Value("-9"), "/", Value("4")));
        SetRoot(3, "Value", Call("OperatorReal", {Call("I2R", {Value("9")}), Value("/"), Value("4.0")}));
        SetRoot(4, "Value", Call("I2R", {Int(Value("9"), "/", Value("4"))}));
        SetRoot(5, "Label", Call("OperatorString", {Value("Count="), Call("I2S", {Var("Count")})}));
        SetRoot(6, "Flag", Call("GetBooleanAnd", {Call("CompareUnit", {Var("U"), Value("!="), Value("null")}),
            Call("CompareInt", {Call("GetUnitTypeId", {Var("U")}), Value("=="), Call("FourCC", {Value("hfoo")})})}));
        SetRoot(7, "U", Call("Null", {})); Root(8, Set(Var("Values", Value("0")), Value("17"))); SetRoot(8, "Count", Var("Values", Value("1")));
        Root(9, Set(Var("Checks", Value("0")), Value("false")));
        SetRoot(9, "Flag", Call("GetBooleanOr", {Call("GetBooleanVariableValue", {Var("Checks", Value("0"))}), Var("Checks", Value("1"))}));
        // G10 的对象工厂在 InitGlobals 中，运行脚本核对两个独立组。
        const auto loop = Eca("ForLoopVarMultiple", {Var("I"), Value("0"), Var("Limit")});
        const auto increment = Set(Var("Count"), Int(Var("Count"), "+", Value("1")));
        const auto limit = Set(Var("Limit"), Value("0"));
        doc.ecaNodes[increment].childId = 0; doc.ecaNodes[limit].childId = 0;
        doc.ecaNodes[loop].children = {increment, limit}; Root(11, loop);
        SetRoot(12, "U", Call("CreateUnit", {Call("Player", {Value("0")}), Value("hfoo"), Value("128.0"), Value("64.0"), Value("270.0")}));
        SetRoot(12, "Item", Call("CreateItem", {Value("ratf"), Value("160.0"), Value("64.0")}));
        Root(13, Eca("TimerEvent", {Value("1.0"), Value("false")}));
        Root(13, Eca("CompareInt", {Var("Count"), Value(">="), Value("0")})); SetRoot(13, "Flag", Value("true"));
        SetRoot(14, "T", Call("CreateTimer", {})); Root(14, Eca("TimerStart", {Var("T"), Value("1.0"), Value("false"), Value("ResearchTimerDone")}));
        Root(15, Eca("ExecuteFunc", {Value("ResearchStart")}));
        SetRoot(16, "V", Var("U")); SetRoot(16, "HandleNumber", Call("GetHandleId", {Var("U")}));
        SetRoot(16, "Flag", Call("CompareUnit", {Var("U"), Value("=="), Var("V")}));
        SetRoot(17, "Roll", Call("GetRandomInt", {Value("1"), Value("6")}));
        SetRoot(17, "Chance", Call("GetRandomReal", {Value("0.0"), Value("1.0")}));
        Root(18, Eca("MapInitializationEvent", {}));
        const auto all = Eca("AndMultiple", {}), nonnegative = Eca("CompareInt", {Var("Count"), Value(">="), Value("0")});
        doc.ecaNodes[nonnegative].childId = 0; doc.ecaNodes[all].children = {nonnegative}; Root(18, all);
        SetRoot(18, "Flag", Value("true")); SetRoot(19, "Flag", Value("true")); SetRoot(20, "Count", Value("11"));
        SetRoot(21, "Value", Call("OperatorReal", {Var("Count"), Value("/"), Var("Quotient")}));
        SetRoot(22, "Quotient", Int(Call("GetRandomInt", {Value("1"), Value("6")}), "/", Call("GetRandomInt", {Value("2"), Value("6")})));
        SetRoot(23, "Label", Value("\"\\中文\r\n\001" "9"));
        SetRoot(24, "Quotient", Int(Var("Count"), "/", Var("Quotient")));
        const auto branch = Eca("IfThenElseMultiple", {});
        const auto compare = Eca("CompareInt", {Var("Count"), Value("=="), Value("0")});
        const auto early = Eca("ReturnAction", {}); const auto other = Set(Var("Flag"), Value("true"));
        doc.ecaNodes[compare].childId = 0; doc.ecaNodes[early].childId = 1; doc.ecaNodes[other].childId = 2;
        doc.ecaNodes[branch].children = {compare, early, other}; Root(25, branch); SetRoot(25, "Count", Value("20"));
        const auto callback = Call("SetVariable", {Var("Flag"), Value("true")});
        doc.ecaNodes[*doc.argumentNodes[callback].callEca].enabled = 0;
        SetRoot(26, "T", Call("CreateTimer", {})); Root(26, Eca("TimerStart", {Var("T"), Value("1.0"), Value("false"), callback}));
        // 来自自然语言T的真实生成样例；运行值/副作用在Lua runner中核对，不只检查helper文本。
        SetRoot(27, "Count", Int(Var("Count"), "+", Var("Quotient")));
        SetRoot(28, "Count", Int(Var("Count"), "-", Var("Quotient")));
        SetRoot(29, "Count", Int(Var("Count"), "*", Var("Quotient")));
        SetRoot(30, "Count", Int(Value("0"), "-", Var("Count")));
        SetRoot(31, "Count", Int(Int(Var("Count"), "+", Value("1")), "/", Value("2")));
        SetRoot(32, "Label", Call("OperatorString", {Call("Null", {}), Var("Label")}));
        SetRoot(33, "Label", Call("OperatorString", {Var("Label"), Call("Null", {})}));
        SetRoot(34, "Label", Call("OperatorString", {Call("Null", {}), Call("Null", {})}));
        SetRoot(35, "Label", Call("OperatorString", {Value(""), Call("Null", {})}));
        Root(36, Set(Var("Values", Call("ArrayIndexProbe", {})), Call("ArrayValueProbe", {})));
        SetRoot(37, "Count", Var("Values", Var("I")));
        Root(38, Set(Var("Values", Var("I")), Call("ArrayValueProbe", {})));
        SetRoot(39, "Label", Var("Labels", Var("I")));
        SetRoot(39, "Value", Var("Reals", Var("I")));
        SetRoot(39, "Flag", Var("Checks", Var("I")));
        SetRoot(40, "Label", Call("OperatorString", {
            Call("StringProbe", {Value("left")}), Call("StringProbe", {Value("right")})}));
    }
};
std::string Diagnostics(const LuaTriggerUnit& result) {
    std::string text; for (const auto& diagnostic : result.diagnostics)
        text += diagnostic.diagnostic.code + ": " + diagnostic.diagnostic.message + "\n";
    return text;
}

void TestLuaBoundGlobals(const std::filesystem::path& root) {
    Fixture fixture(root); fixture.doc.triggers.resize(1);
    const auto global = fixture.Var("gg_unit_fixture");
    fixture.SetRoot(1, "U", global);
    fixture.SetRoot(1, "Count", fixture.Value("FIXTURE_LIMIT"));
    fixture.SetRoot(1, "Label", fixture.Value("FIXTURE_LIMIT")); // 普通字符串不被常量表抢走。
    fixture.profile.constants.push_back({"FIXTURE_LIMIT", "integer"});
    const std::vector<LuaGlobalBinding> globals{{"gg_unit_fixture", "unit"}};
    const auto generated = GenerateLuaTriggerUnit(fixture.doc, fixture.catalog, fixture.profile, {}, globals);
    Require(generated.Succeeded(), Diagnostics(generated));
    Require(generated.requiredGlobals == std::vector<std::string>{"gg_unit_fixture"}
        && generated.requiredConstants == std::vector<std::string>{"FIXTURE_LIMIT"}, "typed map dependencies missing");
    Write(root / "unit.lua", generated.text);
    const auto rejected = [&](const Trigger::TriggerDocument& doc, const LuaTargetProfile& profile,
        const std::vector<LuaGlobalBinding>& bindings, const char* code) {
        const auto result = GenerateLuaTriggerUnit(doc, fixture.catalog, profile, {}, bindings);
        Require(!result.Succeeded() && result.text.empty() && std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
            [&](const auto& diagnostic) { return diagnostic.diagnostic.code == code; }), "typed binding gate: " + std::string(code));
    };
    rejected(fixture.doc, fixture.profile, {}, "LUA_VARIABLE_UNBOUND");
    rejected(fixture.doc, fixture.profile, {{"gg_unit_fixture", "integer"}}, "LUA_TYPE_MISMATCH");
    rejected(fixture.doc, fixture.profile, {{"udg_U", "unit"}}, "LUA_GLOBAL_BINDING");
    auto changed = fixture.doc; changed.argumentNodes[global].value.text += "_unknown";
    rejected(changed, fixture.profile, globals, "LUA_VARIABLE_UNBOUND");
    const auto index = fixture.Value("0");
    changed = fixture.doc; changed.argumentNodes[global].indexArgument = index; changed.argumentNodes[global].insertIndex = 1;
    rejected(changed, fixture.profile, globals, "LUA_ARRAY_SHAPE");
    changed = fixture.doc; changed.ecaNodes[changed.triggers[0].ecas[0]].arguments[0] = global;
    rejected(changed, fixture.profile, globals, "LUA_ASSIGNMENT_TARGET");
    auto profile = fixture.profile; profile.constants[0].type = "unit";
    rejected(fixture.doc, profile, globals, "LUA_TYPE_MISMATCH");
    Write(root / "runner.lua", R"lua(assert(_VERSION == "Lua 5.3")
local original = {identity = "owned-map-unit"}
gg_unit_fixture, FIXTURE_LIMIT = original, 23
assert(loadfile(arg[1], "t", _G))()
Trig_F101_Actions()
assert(udg_U == original and udg_Count == 23 and udg_Label == "FIXTURE_LIMIT")
assert(gg_unit_fixture == original and udg_gg_unit_fixture == nil)
print("typed map global identity and engine constant execution passed")
)lua");
}

void TestLuaObjectCodeRoles(const std::filesystem::path& root) {
    Fixture f(root);
    f.Api("GetTriggerUnit", "unit", {});
    f.Api("GetUnitAbilityLevel", "integer", {"unit", "abilcode"}); f.profile.apis.back().parameters[1] = "integer";
    f.Api("RecordAbility", "nothing", {"abilityalias2"}); f.profile.apis.back().parameters[0] = "integer";
    f.Api("RecordDecimal", "nothing", {"customcode"}); f.profile.apis.back().parameters[0] = "integer";
    Write(root / "ui/base/define.txt", "[TriggerTypes]\nabilcode=0,1,1,Ability,integer\n"
        "abilityalias=0,1,1,Alias,abilcode\nabilityalias2=0,1,1,Alias2,abilityalias\ncustomcode=0,1,1,PlainInteger,integer\n"
        "[TriggerParams]\nAbilityPreset=0,abilcode,'A0E0',Ability\nNumericAbility=0,abilcode,1234,Numeric\nZeroAbility=0,abilcode,0,Zero\n");
    Write(root / "ui/base/action.txt", f.action); Write(root / "ui/base/call.txt", f.call);
    auto catalog = Trigger::UiCatalog::Load(root); Require(catalog.Succeeded(), "Lua object role catalog"); f.catalog = std::move(*catalog.value);
    f.doc.triggers.resize(1); f.doc.variables.clear();
    Trigger::VariableRecord variable; variable.id = f.next++; variable.name.text = "CodeValue";
    variable.typeName.text = "abilityalias2"; variable.hasDefault = 1; variable.value.text = "1234"; f.doc.variables.push_back(variable);
    const auto raw = f.Value("A0E0"); f.Root(1, f.Eca("RecordAbility", {raw}));
    f.Root(1, f.Eca("RecordAbility", {f.Value("1234")}));
    f.Root(1, f.Eca("RecordAbility", {f.Value("AbilityPreset", 0)}));
    f.Root(1, f.Eca("RecordAbility", {f.Value("NumericAbility", 0)}));
    f.Root(1, f.Eca("RecordAbility", {f.Value("ZeroAbility", 0)}));
    f.Root(1, f.Eca("RecordAbility", {f.Var("CodeValue")}));
    const auto decimal = f.Value("1234"); f.Root(1, f.Eca("RecordDecimal", {decimal}));
    f.Root(1, f.Eca("RecordDecimal", {f.Value("0xFF000000")}));
    f.Root(1, f.Eca("RecordDecimal", {f.Value("$FFFFFFFF")}));
    f.Root(1, f.Eca("RecordAbility", {f.Call("GetUnitAbilityLevel", {f.Call("GetTriggerUnit", {}), f.Value("A0E0")})}));
    f.Root(1, f.Eca("RecordAbility", {f.Call("GetUnitAbilityLevel", {f.Value("null"), f.Value("A0E0")})}));
    const auto generated = GenerateLuaTriggerUnit(f.doc, f.catalog, f.profile, {});
    Require(generated.Succeeded(), Diagnostics(generated)); Write(root / "object-code.lua", generated.text);
    Require(generated.text.find("\"A0E0\"") != std::string::npos && generated.text.find("\"1234\"") != std::string::npos,
        "object literal keeps its four bytes through Lua FourCC lowering");
    for (const auto* value : {"A0E", "A0E00", "'A0E0'", "A'00", "A\\00", "0", "null"}) {
        auto invalid = f.doc; invalid.argumentNodes[raw].value.text = value;
        const auto result = GenerateLuaTriggerUnit(invalid, f.catalog, f.profile, {});
        Require(!result.Succeeded() && result.text.empty() && std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
            [](const auto& diagnostic) { return diagnostic.diagnostic.code == "LUA_LITERAL_UNTRANSLATED" || diagnostic.diagnostic.code == "LUA_TYPE_MISMATCH"; }),
            "Lua invalid object value remains rejected at its source");
    }
    auto invalid = f.doc; invalid.argumentNodes[decimal].value.text = "A0E0";
    Require(!GenerateLuaTriggerUnit(invalid, f.catalog, f.profile, {}).Succeeded(), "unknown code suffix remains decimal integer");
    Write(root / "object-code-runner.lua", R"lua(assert(_VERSION == "Lua 5.3")
local seen, decimal, source = {}, false, {}
function FourCC(text)
    assert(#text == 4)
    local value = 0
    for i = 1, 4 do value = value * 256 + text:byte(i) end
    return value < 2147483648 and value or value - 4294967296
end
function GetTriggerUnit() return source end
function GetUnitAbilityLevel(unit, ability)
    assert((unit == source or unit == nil) and ability == FourCC("A0E0"))
    return unit == source and 7 or 8
end
function RecordAbility(value) seen[#seen + 1] = value end
function RecordDecimal(value)
    assert(value == 1234 or value == -16777216 or value == -1)
    source[#source + 1] = value; decimal = true
end
assert(loadfile(arg[1], "t", _G))()
InitGlobals()
assert(udg_CodeValue == FourCC("1234"))
Trig_F101_Actions()
local expected = {FourCC("A0E0"), FourCC("1234"), FourCC("A0E0"), 1234, 0, FourCC("1234"), 7, 8}
assert(#seen == #expected and decimal)
assert(#source == 3 and source[1] == 1234 and source[2] == -16777216 and source[3] == -1)
for i, value in ipairs(expected) do assert(seen[i] == value, "object source role changed at " .. i) end
print("object rawcode, multi-level alias, numeric preset, decimal and nil handle runtime passed")
)lua");
}
}

int main(int argc, char** argv) {
    try {
        const auto root = argc > 1 ? std::filesystem::absolute(argv[1]) : std::filesystem::temp_directory_path()
            / ("forge-lua-unit-" + std::to_string(GetCurrentProcessId()));
        Fixture fixture(root); fixture.Samples();
        TestLuaObjectCodeRoles(root / "object-code");
        TestLuaBoundGlobals(root / "bound-globals");
        TestLuaTriggerCandidate(fixture.catalog, fixture.profile);
        const auto generated = GenerateLuaTriggerUnit(fixture.doc, fixture.catalog, fixture.profile, fixture.callbacks);
        Require(generated.Succeeded(), Diagnostics(generated));
        Require(generated.targetSha256.size() == 64 && generated.scriptSha256.size() == 64, "result fingerprints missing");
        Require(GenerateLuaTriggerUnit(fixture.doc, fixture.catalog, fixture.profile, fixture.callbacks).text == generated.text, "repeat differs");
        Write(root / "unit.lua", generated.text);
        for (const auto& span : generated.sourceSpans)
            Require(span.nodeId && span.byteOffset <= generated.text.size()
                && span.byteLength <= generated.text.size() - span.byteOffset, "source range escapes actual generated bytes");
        const auto loopNode = std::find_if(fixture.doc.ecaNodes.begin(), fixture.doc.ecaNodes.end(),
            [](const auto& node) { return node.name.text == "ForLoopVarMultiple"; });
        Require(loopNode != fixture.doc.ecaNodes.end(), "loop fixture absent");
        const auto loop = std::find_if(generated.sourceSpans.begin(), generated.sourceSpans.end(),
            [&](const auto& span) { return span.nodeId == loopNode->id; });
        Require(loop != generated.sourceSpans.end()
            && generated.text.substr(loop->byteOffset, loop->byteLength).find("while true do") != std::string::npos,
            "nested loop source was not shifted with its actions buffer");
        const auto reject = [&](Trigger::TriggerDocument document, const char* code) {
            const auto result = GenerateLuaTriggerUnit(document, fixture.catalog, fixture.profile, fixture.callbacks);
            Require(!result.Succeeded() && result.text.empty() && result.sourceSpans.empty() && std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
                [&](const auto& item) { return item.diagnostic.code == code; }), Diagnostics(result) + " expected " + code);
        };
        auto changed = fixture.doc; changed.wct.customCode.text = "function hidden takes nothing returns nothing\nendfunction";
        reject(changed, "LUA_SOURCE_UNTRANSLATED");
        changed = fixture.doc; changed.variables[1].name = changed.variables[0].name; reject(changed, "LUA_VARIABLE_INVALID");
        changed = fixture.doc; changed.ecaNodes[changed.triggers[0].ecas[0]].arguments[1] = 0xffffffffu; reject(changed, "LUA_ARGUMENT_INDEX");
        changed = fixture.doc;
        const auto execute = changed.triggers[14].ecas[0];
        changed.argumentNodes[changed.ecaNodes[execute].arguments[0]].type = 1;
        changed.argumentNodes[changed.ecaNodes[execute].arguments[0]].value.text = "Label";
        reject(changed, "LUA_DYNAMIC_ENTRY_UNRESOLVED");
        changed = fixture.doc;
        const auto assignment = changed.triggers[0].ecas[0];
        const auto expression = changed.ecaNodes[assignment].arguments[1];
        changed.ecaNodes[*changed.argumentNodes[expression].callEca].arguments[0] = expression;
        reject(changed, "LUA_GRAPH_BUDGET_OR_CYCLE");
        changed = fixture.doc; changed.triggers[0].type = 42; reject(changed, "LUA_TRIGGER_KIND");
        changed = fixture.doc; changed.triggers[0].enabled = 0; changed.triggers[0].customTextFlag = 1;
        reject(changed, "LUA_SOURCE_UNTRANSLATED");
        changed = fixture.doc;
        changed.argumentNodes[changed.ecaNodes[*changed.argumentNodes[expression].callEca].arguments[0]].value.text = "1); injected()";
        reject(changed, "LUA_LITERAL_UNTRANSLATED");
        auto profile = fixture.profile; profile.apis.erase(profile.apis.begin());
        const auto missing = GenerateLuaTriggerUnit(fixture.doc, fixture.catalog, profile, fixture.callbacks);
        Require(!missing.Succeeded() && missing.text.empty(), "missing native accepted");
        profile = fixture.profile; profile.apis.push_back({"math", "nothing", {}});
        Require(!GenerateLuaTriggerUnit(fixture.doc, fixture.catalog, profile, fixture.callbacks).Succeeded(), "runtime global collision accepted");
        changed = fixture.doc;
        const auto largeAssignment = changed.triggers[22].ecas[0];
        changed.argumentNodes[changed.ecaNodes[largeAssignment].arguments[1]].value.text.assign(65536, 'x');
        changed.triggers[22].ecas.assign(140, largeAssignment);
        reject(changed, "LUA_OUTPUT_BUDGET");
        Script::detail::ProcessRequest request;
        request.executable = Tests::LuaTestExecutable();
        request.workingDirectory = root;
        request.arguments = {L"-E", std::filesystem::path(LUA_TEST_RUNNER).wstring(), (root / "unit.lua").wstring()};
        request.outputFile = root / "runtime.log"; request.timeout = std::chrono::seconds(20);
        const auto execution = Script::detail::RunProcess(request);
        Require(execution.Succeeded(), execution.error + execution.output);
        std::cout << execution.output;
        request.arguments = {L"-E", (root / "object-code/object-code-runner.lua").wstring(), (root / "object-code/object-code.lua").wstring()};
        request.outputFile = root / "object-code/runtime.log";
        const auto objectExecution = Script::detail::RunProcess(request);
        Require(objectExecution.Succeeded(), objectExecution.error + objectExecution.output);
        std::cout << objectExecution.output;
        request.arguments = {L"-E", (root / "bound-globals/runner.lua").wstring(), (root / "bound-globals/unit.lua").wstring()};
        request.outputFile = root / "bound-globals/runtime.log";
        const auto boundExecution = Script::detail::RunProcess(request);
        Require(boundExecution.Succeeded(), boundExecution.error + boundExecution.output);
        std::cout << boundExecution.output;
        if (argc <= 1 && root.filename().string().starts_with("forge-lua-unit-")) std::filesystem::remove_all(root);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
