#include "War3MapCore/Jass/JassChecker.h"
#include "War3MapCore/Jass/Preprocessor.h"
#include "War3MapCore/Jass/VJass.h"
#include "War3MapCore/Script/MapScript.h"
#include "War3MapCore/Script/ScriptFrameworkInputs.h"
#include "War3MapCore/Script/ScriptPipeline.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// 原创合成资源：公开 GUI 编辑/生成、两条实际注入入口、内置预处理及严格检查。
// 不读取安装 YDWE，不复制其宏实现，不把编译通过当作游戏运行证明。
namespace {
using namespace War3Map;
using namespace War3Map::Script;
using namespace War3Map::Trigger;

struct ObjectKind { const char* constant; const char* type; const char* table; const char* rawcode; };
constexpr std::array<ObjectKind, 7> kObjects{{
    {"ABILITY", "abilcode", "ability", "A001"}, {"BUFF", "buffcode", "buff", "B002"},
    {"UNIT", "unitcode", "unit", "h003"}, {"ITEM", "itemcode", "item", "I004"},
    {"UPGRADE", "techcode", "upgrade", "R005"}, {"DOODAD", "doodadcode", "doodad", "D006"},
    {"DESTRUCTABLE", "destructablecode", "destructable", "B007"},
}};
struct ValueKind { const char* suffix; const char* type; const char* conversion; };
constexpr std::array<ValueKind, 3> kValues{{
    {"Integer", "integer", "S2I"}, {"Real", "real", "S2R"}, {"String", "string", ""},
}};
enum class Fault { None, MissingLibrary, MissingAlias, MissingMacro };

void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
std::string Details(const std::vector<Diagnostic>& diagnostics) {
    std::string result;
    for (const auto& diagnostic : diagnostics) result += diagnostic.code + ": " + diagnostic.message + "\n";
    return result;
}
bool HasCode(const std::vector<Diagnostic>& diagnostics, std::string_view code) {
    for (const auto& diagnostic : diagnostics) if (diagnostic.code == code) return true;
    return false;
}
void WriteText(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    Require(output.good(), "cannot write original fixture: " + path.string());
}
std::size_t Count(std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t at = 0; (at = text.find(needle, at)) != std::string_view::npos; at += needle.size()) ++count;
    return count;
}
std::string Compact(std::string_view text) {
    std::string result;
    for (const char ch : text) if (std::string_view(" \r\n\t").find(ch) == std::string_view::npos) result += ch;
    return result;
}
std::string Preset(std::size_t index) {
    return "objecttype0" + std::to_string(index) + "_" + kObjects[index].type;
}
std::string IdCall(std::size_t index) { return "ProbeObjectId" + std::to_string(index); }
std::string KeyCall(std::size_t index) { return "ProbePropertyKey" + std::to_string(index); }

struct Fixture {
    std::filesystem::path tool, ui, map, include, libraries;
    std::string common;
};
Fixture MakeFixture(const std::filesystem::path& root, Fault fault = Fault::None) {
    Fixture result{root / "framework", {}, root / "map", {}, {}, {}};
    result.ui = result.tool / "share/mpq";
    result.include = result.tool / "plugin/wave/include";
    result.libraries = result.tool / "jass";
    std::filesystem::create_directories(result.map);
    WriteText(result.ui / "config", "probe\n");
    std::string define = "[TriggerTypes]\ninteger=0,1,1,Integer,integer\nreal=0,1,1,Real,real\n"
        "string=0,1,1,String,string\nobjecttype=0,0,0,ObjectType\nNull=0,0,0,SelectedType\n";
    for (const auto& object : kObjects) define += std::string(object.type) + "=0,1,1,Code,integer\n";
    define += "[TriggerParams]\n";
    for (std::size_t index = 0; index < kObjects.size(); ++index)
        define += Preset(index) + "=0,objecttype,YDWE_OBJECT_TYPE_" + kObjects[index].constant + ",Object\n";
    WriteText(result.ui / "probe/define.txt", define);
    std::string calls, actions;
    for (const auto& value : kValues) {
        calls += "[YDWEGetObjectProperty" + std::string(value.suffix) + "]\nreturns=" + value.type
            + "\n[[.args]]\ntype=objecttype\n[[.args]]\ntype=Null\n[[.args]]\ntype=string\n";
        actions += "[Record" + std::string(value.suffix) + "]\n[[.args]]\ntype=" + value.type + "\n";
    }
    result.common = "type trigger extends handle\ntype triggeraction extends handle\ntype weathereffect extends handle\n"
        "native CreateTrigger takes nothing returns trigger\n"
        "native TriggerAddAction takes trigger t, code action returns triggeraction\n"
        "native DisableTrigger takes trigger t returns nothing\n"
        "native ConditionalTriggerExecute takes trigger t returns nothing\n"
        "native InitBlizzard takes nothing returns nothing\n"
        "native SetMapMusic takes string music, boolean looping, integer index returns nothing\n"
        "native SetPlayers takes integer players returns nothing\nnative SetTeams takes integer teams returns nothing\n"
        "native SetGamePlacement takes integer placement returns nothing\n"
        "native InitGenericPlayerSlots takes nothing returns nothing\n"
        "native ExecuteFunc takes string name returns nothing\n"
        "native I2S takes integer value returns string\nnative S2I takes string value returns integer\n"
        "native S2R takes string value returns real\n"
        "native RecordInteger takes integer value returns nothing\nnative RecordReal takes real value returns nothing\n"
        "native RecordString takes string value returns nothing\n"
        "globals\nconstant integer MAP_PLACEMENT_USE_MAP_SETTINGS=0\nendglobals\n";
    for (std::size_t index = 0; index < kObjects.size() * kValues.size(); ++index) {
        calls += "[" + IdCall(index) + "]\nreturns=" + kObjects[index / kValues.size()].type + "\n["
            + KeyCall(index) + "]\nreturns=string\n";
        result.common += "native " + IdCall(index) + " takes nothing returns integer\nnative "
            + KeyCall(index) + " takes nothing returns string\n";
    }
    WriteText(result.ui / "probe/call.txt", calls);
    WriteText(result.ui / "probe/action.txt", actions);
    WriteText(result.libraries / "system/ht/common.j", result.common);
    WriteText(result.libraries / "system/ht/blizzard.j", "// original empty helper base\n");
    WriteText(result.include / "WaveForce.i", "// original empty force include\n");
    // 自己设计的表名宏分派；没有使用 YDWE 的 switch/if 宏实现。
    std::string header = "#ifndef PROBE_PROPERTY_HEADER\n#define PROBE_PROPERTY_HEADER\n"
        "#define ProbeJoinRaw(a,b) a ## b\n#define ProbeJoin(a,b) ProbeJoinRaw(a,b)\n"
        "#define ProbeTable(kind) ProbeJoin(ProbeTable_,kind)\n";
    for (const auto& object : kObjects)
        header += "#define ProbeTable_YDWE_OBJECT_TYPE_" + std::string(object.constant)
            + " \"(require'jass.slk')." + object.table + "\"\n";
    header += "#define ProbeRead(kind,id,key) EXExecuteScript(ProbeTable(kind) + \"[\" + I2S(id) + \"].\" + key)\n"
        "#define YDWEGetObjectPropertyInteger(kind,id,key) S2I(ProbeRead(kind,id,key))\n"
        "#define YDWEGetObjectPropertyReal(kind,id,key) S2R(ProbeRead(kind,id,key))\n"
        "#define YDWEGetObjectPropertyString(kind,id,key) ProbeRead(kind,id,key)\n#endif\n";
    WriteText(result.include / "YDTrigger/YDTrigger.h", fault == Fault::MissingMacro ? "// no property macros\n" : header);
    WriteText(result.include / "YDTrigger/Globals.h", "// original empty global header\n");
    WriteText(result.include / "YDTrigger/Function.h", "// original empty function header\n");
    WriteText(result.libraries / "Property.cfg", fault == Fault::MissingAlias ? "UnusedPropertyEntry\n" :
        "YDWEGetObjectPropertyInteger\nYDWEGetObjectPropertyReal\nYDWEGetObjectPropertyString\n");
    if (fault != Fault::MissingLibrary)
        WriteText(result.libraries / "Property.j", "#ifndef PROBE_PROPERTY_DECLARED\n#define PROBE_PROPERTY_DECLARED\n"
            "library ProbePropertyLibrary\nnative EXExecuteScript takes string script returns string\nendlibrary\n#endif\n");
    WriteText(result.libraries / "LeakMonitor.cfg", "LeakMonitor\n");
    WriteText(result.libraries / "LeakMonitor.j", "library ProbeUnwantedLeakMonitor\nendlibrary\n");
    return result;
}

GraphArgumentValue Scalar(int kind, std::string value) {
    GraphArgumentValue result; result.scalar = {kind, std::move(value)}; return result;
}
std::shared_ptr<GraphEcaValue> Node(std::string name, UiDefinitionKind kind, std::vector<GraphArgumentValue> args = {}) {
    auto result = std::make_shared<GraphEcaValue>();
    result->function = std::move(name); result->kind = kind; result->arguments = std::move(args); return result;
}
GraphArgumentValue Call(std::string name, std::vector<GraphArgumentValue> args = {}) {
    GraphArgumentValue result; result.call = Node(std::move(name), UiDefinitionKind::Call, std::move(args)); return result;
}
TriggerDocument MakeDocument(const UiCatalog& catalog) {
    auto empty = WtgWctCodec{}.CreateEmpty(catalog); Require(empty.Succeeded(), "empty original pair: " + Details(empty.diagnostics));
    auto document = std::move(*empty.value);
    auto category = TriggerDirectoryEditor::AddCategory(document, "Property"); Require(category.Succeeded(), "category");
    auto trigger = TriggerDocumentEditor::AddTrigger(document, *category.value, "PropertyMatrix"); Require(trigger.Succeeded(), "trigger");
    for (std::size_t object = 0; object < kObjects.size(); ++object) {
        for (std::size_t value = 0; value < kValues.size(); ++value) {
            const auto index = object * kValues.size() + value;
            for (const bool dynamic : {false, true}) {
                const auto property = Call("YDWEGetObjectProperty" + std::string(kValues[value].suffix), {
                    Scalar(0, Preset(object)), dynamic ? Call(IdCall(index)) : Scalar(3, kObjects[object].rawcode),
                    dynamic ? Call(KeyCall(index)) : Scalar(3, "field" + std::to_string(index))});
                auto binding = CaptureTriggerGraphBinding(document, catalog); Require(binding.Succeeded(), "graph binding");
                GraphParentExpectation parent{GraphParentKind::TriggerEcas, {GraphRecordKind::Trigger, *trigger.value, {}}, {}, {}};
                for (const auto child : document.triggers.front().ecas)
                    parent.orderedChildren.push_back({GraphRecordKind::Eca, document.ecaNodes[child].id, {}});
                TriggerGraphEditBatch batch{*binding.value, {GraphInsertSubtree{parent, {},
                    Node("Record" + std::string(kValues[value].suffix), UiDefinitionKind::Action, {property})}}};
                const auto edited = ApplyTriggerGraphEditBatch(document, catalog, batch);
                Require(edited.Succeeded(), "public typed property edit: " + Details(edited.diagnostics));
            }
        }
    }
    const auto encoded = WtgWctCodec{}.Serialize(document, catalog); Require(encoded.Succeeded(), "matrix WTG/WCT serialize");
    auto parsed = WtgWctCodec{}.Parse(encoded.value->wtg, encoded.value->wct, catalog);
    Require(parsed.Succeeded(), "matrix WTG/WCT reopen"); return std::move(*parsed.value);
}
Result<ScriptFrameworkInputs> CaptureOwned(const Fixture& fixture, const UiCatalog& catalog) {
    const auto layout = ResolveScriptToolchainLayout({fixture.tool, 24, 127});
    Require(layout.Succeeded(), "original legacy layout: " + Details(layout.diagnostics));
    auto profile = MakeLegacyFrameworkProfile(*layout.value, catalog, fixture.ui, fixture.map);
    Require(profile.Succeeded(), "original framework profile: " + Details(profile.diagnostics));
    return CaptureScriptFrameworkInputs(*profile.value, *layout.value, catalog);
}
struct PipelineResult {
    std::string injected, preprocessed, translated;
    std::vector<std::filesystem::path> libraries;
    Jass::JassCheckResult checked;
};
PipelineResult RunPipeline(std::string_view generated, const Fixture& fixture,
    const ScriptFrameworkInputs* owned = nullptr) {
    PipelineResult result;
    if (owned) {
        Require(VerifyScriptFrameworkInputs(*owned, owned->layout).Succeeded(), "owned inputs still match real files");
        auto injected = InjectScriptFramework(generated, *owned, result.libraries);
        Require(injected.Succeeded(), "owned injection: " + Details(injected.diagnostics));
        result.injected = std::move(*injected.value);
    } else {
        std::vector<Diagnostic> diagnostics;
        const auto table = ScanInjectTable({fixture.libraries}, true, diagnostics);
        result.injected = InjectCode(std::string(generated), table, diagnostics, &result.libraries);
        Require(diagnostics.empty(), "ordinary injection: " + Details(diagnostics));
    }
    Jass::PreprocessOptions options; options.systemIncludes.push_back(fixture.include);
    auto pp = Jass::Preprocess(result.injected, "war3map.j", fixture.map, options);
    Require(pp.Succeeded(), "actual builtin preprocess: " + Details(pp.diagnostics));
    result.preprocessed = std::move(pp.text);
    Jass::VJassOptions voptions; voptions.baseScripts.push_back({"common.j", fixture.common});
    auto translated = Jass::TranslateVJass(result.preprocessed, "war3map.j", voptions);
    Require(translated.Succeeded(), "actual builtin vJass: " + Details(translated.diagnostics) + translated.unsupported);
    result.translated = std::move(translated.text);
    const std::array<Jass::JassSource, 2> sources{{{"common.j", fixture.common}, {"war3map.j", result.translated}}};
    Jass::JassCheckOptions checks; checks.threads = 1;
    result.checked = Jass::CheckJass(sources, checks); return result;
}
void VerifyMatrix(std::string_view text, bool expanded) {
    const auto compact = Compact(text);
    for (std::size_t object = 0; object < kObjects.size(); ++object) {
        for (std::size_t value = 0; value < kValues.size(); ++value) {
            const auto index = object * kValues.size() + value;
            for (const bool dynamic : {false, true}) {
                const std::string id = dynamic ? IdCall(index) + "()" : "'" + std::string(kObjects[object].rawcode) + "'";
                const std::string key = dynamic ? KeyCall(index) + "()" : "\"field" + std::to_string(index) + "\"";
                std::string expression;
                if (expanded) {
                    expression = "EXExecuteScript(\"(require'jass.slk')." + std::string(kObjects[object].table)
                        + "\"+\"[\"+I2S(" + id + ")+\"].\"+" + key + ")";
                    if (*kValues[value].conversion) expression = std::string(kValues[value].conversion) + "(" + expression + ")";
                } else {
                    expression = "YDWEGetObjectProperty" + std::string(kValues[value].suffix)
                        + "(YDWE_OBJECT_TYPE_" + kObjects[object].constant + "," + id + "," + key + ")";
                }
                Require(Count(compact, "callRecord" + std::string(kValues[value].suffix) + "(" + expression + ")") == 1,
                    "missing or repeated typed matrix expression: " + expression);
            }
            Require(Count(compact, IdCall(index) + "()") == 1 && Count(compact, KeyCall(index) + "()") == 1,
                "object/property expressions must each be evaluated once");
        }
    }
}

void TestLexicalNegatives(const Fixture& fixture, const ScriptFrameworkInputs& owned) {
    const std::string ignored = "// YDWEGetObjectPropertyInteger\n/*YDWEGetObjectPropertyReal*/\n"
        "globals\nstring note=\"YDWEGetObjectPropertyString\"\ninteger object='YDWE'\nendglobals\n"
        "call PrefixYDWEGetObjectPropertyString()\ncall YDWEGetObjectPropertyRealSuffix()\n"
        "call MHDebug_EnableLeakMonitor()\n";
    std::vector<Diagnostic> diagnostics;
    const auto table = ScanInjectTable({fixture.libraries}, true, diagnostics);
    std::vector<std::filesystem::path> libraries;
    Require(InjectCode(ignored, table, diagnostics, &libraries) == ignored && libraries.empty() && diagnostics.empty(),
        "ordinary comments, strings, rawcodes and longer names must not inject; MemHack remains distinct");
    const auto result = InjectScriptFramework(ignored, owned, libraries);
    Require(result.Succeeded() && *result.value == ignored && libraries.empty(), "owned lexical negative must not inject");
}

void Run(const std::filesystem::path& root) {
    const auto fixture = MakeFixture(root / "valid");
    const auto catalog = UiCatalog::Load(fixture.ui); Require(catalog.Succeeded(), "original property catalog");
    auto document = MakeDocument(*catalog.value);
    MapScriptInputs inputs; inputs.catalog = &*catalog.value; inputs.triggers = &document;
    const auto generated = GenerateMapScript(inputs);
    Require(generated.Succeeded(), "public matrix generation: " + Details(generated.diagnostics));
    Require(generated.text.find("EXExecuteScript") == std::string::npos
        && generated.text.find("/*YDWEGetObjectProperty") == std::string::npos,
        "generation preserves actual macros without EX lowering or comment dependency markers");
    VerifyMatrix(generated.text, false);
    const auto owned = CaptureOwned(fixture, *catalog.value);
    Require(owned.Succeeded(), "capture actual original sources: " + Details(owned.diagnostics));
    const auto ordinary = RunPipeline(generated.text, fixture);
    const auto captured = RunPipeline(generated.text, fixture, &*owned.value);
    for (const auto* result : {&ordinary, &captured}) {
        Require(result->libraries.size() == 1 && result->libraries.front().filename() == "Property.j",
            "three aliases and repeated GUI nodes select one real source library");
        Require(Count(result->injected, "native EXExecuteScript takes string script returns string") == 1
            && Count(Compact(result->translated), "nativeEXExecuteScripttakesstringscriptreturnsstring") == 1,
            "injected and translated output have exactly one correct native from the selected source");
        VerifyMatrix(result->preprocessed, true);
        Require(result->checked.Succeeded() && result->checked.errorsIgnored == 0,
            "actual strict final JASS check: " + Details(result->checked.diagnostics));
    }
    Require(Compact(ordinary.preprocessed) == Compact(captured.preprocessed), "ordinary and owned injection preserve equal semantics");
    TestLexicalNegatives(fixture, *owned.value);
    for (const auto fault : {Fault::MissingAlias, Fault::MissingMacro, Fault::MissingLibrary}) {
        const auto broken = MakeFixture(root / ("fault-" + std::to_string(static_cast<int>(fault))), fault);
        const auto brokenCatalog = UiCatalog::Load(broken.ui); Require(brokenCatalog.Succeeded(), "fault catalog");
        const auto brokenOwned = CaptureOwned(broken, *brokenCatalog.value);
        if (fault == Fault::MissingLibrary) {
            Require(!brokenOwned.Succeeded() && HasCode(brokenOwned.diagnostics, "INJECTION_SOURCE_LIBRARY_MISSING"),
                "owned capture rejects a cfg with no corresponding source library");
            std::vector<Diagnostic> diagnostics;
            const auto table = ScanInjectTable({broken.libraries}, true, diagnostics);
            const auto output = InjectCode(generated.text, table, diagnostics);
            Require(HasCode(diagnostics, "INJECT_FILE_UNREADABLE") && output.find("native EXExecuteScript") == std::string::npos,
                "ordinary missing library is an error and never synthesizes a declaration");
            continue;
        }
        Require(brokenOwned.Succeeded(), "fault source capture remains structurally valid");
        for (const bool useOwned : {false, true}) {
            const auto result = RunPipeline(generated.text, broken, useOwned ? &*brokenOwned.value : nullptr);
            Require(!result.checked.Succeeded() && HasCode(result.checked.diagnostics, "JASS_UNDECLARED")
                && result.checked.errorsIgnored == 0, "missing alias/macro fails strict checking without bypass");
            if (fault == Fault::MissingAlias)
                Require(result.libraries.empty() && result.preprocessed.find("native EXExecuteScript") == std::string::npos,
                    "unregistered alias does not fabricate an EX declaration");
            else
                Require(result.preprocessed.find("YDWEGetObjectPropertyInteger(") != std::string::npos,
                    "missing header macro remains unresolved rather than receiving a generated substitute");
        }
    }
    for (auto& argument : document.argumentNodes) {
        if (argument.type == 3 && argument.value.text == kObjects.front().rawcode) {
            argument.value.text = "A'01"; break;
        }
    }
    const auto invalid = GenerateMapScript(inputs);
    Require(!invalid.Succeeded() && HasCode(invalid.diagnostics, "TRIGGER_LITERAL_INVALID"),
        "rawcode inference still rejects malformed object IDs at generation");
}
} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / ("war3-object-property-tests-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        Require(std::filesystem::create_directory(root), "test root must be a new owned directory");
        Run(root);
        std::filesystem::remove_all(root);
        std::cout << "ObjectProperty: 7 types x 3 conversions x 2 argument forms, both injection paths and strict negative gates passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << "\nOriginal test fixtures retained at " << root.string() << '\n';
        return 1;
    }
}
