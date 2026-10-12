#include "War3MapCore/Jass/JassChecker.h"
#include "War3MapCore/Jass/Preprocessor.h"
#include "War3MapCore/Jass/VJass.h"
#include "War3MapCore/Script/MapScript.h"
#include "War3MapCore/Script/ScriptFrameworkInputs.h"
#include "War3MapCore/Script/ScriptPipeline.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Trigger/UiCallbackActions.h"

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

// 原创合成函数库：UI 定义比 J 声明少末尾 code 参数的动作按回调块识别、生成并通过严格检查。
// 不读取安装 YDWE 或第三方函数库，不把编译通过当作游戏运行证明。
namespace {
using namespace War3Map;
using namespace War3Map::Script;
using namespace War3Map::Trigger;

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

void TestScanner() {
    const std::string text =
        "library ProbeScan\r\n"
        "    private function Hidden takes integer a, code c returns nothing // trailing\r\n"
        "    endfunction\r\n"
        "    native Plain takes nothing returns nothing\r\n"
        "// function Commented takes code c returns nothing\r\n"
        "    constant native Konst takes real x, boolean y returns real\r\n"
        "    function interface Shape takes integer a returns nothing\r\n"
        "    set s = \"function Quoted takes code c returns nothing\"\n"
        "endlibrary";
    const auto found = ScanUiJassDeclarations(text, "scan.j");
    Require(found.size() == 3, "scanner finds exactly the three real declarations");
    Require(found[0].name == "Hidden" && found[0].parameterTypes == std::vector<std::string>{"integer", "code"} && found[0].line == 2,
        "private function with trailing comment keeps parameter types and line");
    Require(found[1].name == "Plain" && found[1].parameterTypes.empty(), "takes nothing has no parameters");
    Require(found[2].name == "Konst" && found[2].parameterTypes.size() == 2, "constant native recognised");
}

struct Fixture {
    std::filesystem::path tool, ui, map, include, libraries;
    std::string common;
};
Fixture MakeFixture(const std::filesystem::path& root) {
    Fixture result{root / "framework", {}, root / "map", {}, {}, {}};
    result.ui = result.tool / "share/mpq";
    result.include = result.tool / "plugin/wave/include";
    result.libraries = result.tool / "jass";
    std::filesystem::create_directories(result.map);
    WriteText(result.ui / "config", "probe\n");
    WriteText(result.ui / "probe/define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer,integer\n");
    WriteText(result.ui / "probe/action.txt",
        "[ProbeModuleCreate]\n[[.args]]\ntype=integer\n[[.args]]\ntype=integer\n"
        "[RecordInteger]\n[[.args]]\ntype=integer\n"
        "[ProbePlain]\n[[.args]]\ntype=integer\n"
        "[ProbeCodeFirst]\n[[.args]]\ntype=integer\n"
        "[ProbeAmbiguous]\n[[.args]]\ntype=integer\n");
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
        "native RecordInteger takes integer value returns nothing\n"
        "native ProbePlain takes integer value returns nothing\n"
        "globals\nconstant integer MAP_PLACEMENT_USE_MAP_SETTINGS=0\nendglobals\n";
    WriteText(result.libraries / "system/ht/common.j", result.common);
    WriteText(result.libraries / "system/ht/blizzard.j", "// original empty helper base\n");
    WriteText(result.include / "WaveForce.i", "// original empty force include\n");
    WriteText(result.include / "YDTrigger/YDTrigger.h", "// original empty trigger header\n");
    WriteText(result.include / "YDTrigger/Globals.h", "// original empty global header\n");
    WriteText(result.include / "YDTrigger/Function.h", "// original empty function header\n");
    // 注入入口在根目录，真实声明在子目录（与按 #include 组织的函数库一致）。
    WriteText(result.libraries / "ProbeModule.cfg", "ProbeModuleCreate\n");
    WriteText(result.libraries / "ProbeModule.j", "#include \"addon/module.j\"\n");
    WriteText(result.libraries / "addon/module.j", "#ifndef PROBE_MODULE_DECLARED\n#define PROBE_MODULE_DECLARED\n"
        "library ProbeModuleLibrary\n"
        "    function ProbeModuleCreate takes integer parentKey, integer childKey, code callback returns nothing\n"
        "        call RecordInteger(parentKey + childKey)\n"
        "    endfunction\n"
        "endlibrary\n#endif\n");
    // 不是回调块：code 不在末尾；同名声明参数不一致。这些文件只参与识别，不被注入。
    WriteText(result.libraries / "notes/negative.j",
        "native ProbeCodeFirst takes code callback, integer value returns nothing\n"
        "native ProbeAmbiguous takes integer value, code callback returns nothing\n");
    WriteText(result.libraries / "notes/other.j", "native ProbeAmbiguous takes integer value, code callback, integer more returns nothing\n");
    return result;
}

GraphArgumentValue Scalar(int kind, std::string value) {
    GraphArgumentValue result; result.scalar = {kind, std::move(value)}; return result;
}
std::shared_ptr<GraphEcaValue> Action(std::string name, std::vector<GraphArgumentValue> args = {},
    std::vector<std::shared_ptr<const GraphEcaValue>> children = {}, bool enabled = true) {
    auto result = std::make_shared<GraphEcaValue>();
    result->function = std::move(name); result->kind = UiDefinitionKind::Action; result->arguments = std::move(args);
    result->children = std::move(children); result->enabled = enabled; return result;
}
std::shared_ptr<GraphEcaValue> Child(std::shared_ptr<GraphEcaValue> node) { node->childBranch = 0; return node; }

TriggerDocument MakeDocument(const UiCatalog& catalog, const std::vector<std::shared_ptr<const GraphEcaValue>>& actions) {
    auto empty = WtgWctCodec{}.CreateEmpty(catalog); Require(empty.Succeeded(), "empty original pair: " + Details(empty.diagnostics));
    auto document = std::move(*empty.value);
    auto category = TriggerDirectoryEditor::AddCategory(document, "Probe"); Require(category.Succeeded(), "category");
    auto trigger = TriggerDocumentEditor::AddTrigger(document, *category.value, "Callback"); Require(trigger.Succeeded(), "trigger");
    for (const auto& action : actions) {
        auto binding = CaptureTriggerGraphBinding(document, catalog); Require(binding.Succeeded(), "graph binding");
        GraphParentExpectation parent{GraphParentKind::TriggerEcas, {GraphRecordKind::Trigger, *trigger.value, {}}, {}, {}};
        for (const auto child : document.triggers.front().ecas)
            parent.orderedChildren.push_back({GraphRecordKind::Eca, document.ecaNodes[child].id, {}});
        TriggerGraphEditBatch batch{*binding.value, {GraphInsertSubtree{parent, {}, action}}};
        const auto edited = ApplyTriggerGraphEditBatch(document, catalog, batch);
        Require(edited.Succeeded(), "public graph edit: " + Details(edited.diagnostics));
    }
    const auto encoded = WtgWctCodec{}.Serialize(document, catalog); Require(encoded.Succeeded(), "WTG/WCT serialize");
    auto parsed = WtgWctCodec{}.Parse(encoded.value->wtg, encoded.value->wct, catalog);
    Require(parsed.Succeeded(), "WTG/WCT reopen keeps callback children"); return std::move(*parsed.value);
}

Jass::JassCheckResult RunPipeline(std::string_view generated, const Fixture& fixture, std::string& translatedText) {
    std::vector<Diagnostic> diagnostics;
    const auto table = ScanInjectTable({fixture.libraries}, true, diagnostics);
    std::vector<std::filesystem::path> libraries;
    const auto injected = InjectCode(std::string(generated), table, diagnostics, &libraries);
    Require(diagnostics.empty() && libraries.size() == 1, "module library injected once: " + Details(diagnostics));
    Jass::PreprocessOptions options; options.systemIncludes.push_back(fixture.include);
    auto pp = Jass::Preprocess(injected, "war3map.j", fixture.libraries, options);
    Require(pp.Succeeded(), "actual builtin preprocess: " + Details(pp.diagnostics));
    Jass::VJassOptions voptions; voptions.baseScripts.push_back({"common.j", fixture.common});
    auto translated = Jass::TranslateVJass(pp.text, "war3map.j", voptions);
    Require(translated.Succeeded(), "actual builtin vJass: " + Details(translated.diagnostics) + translated.unsupported);
    translatedText = translated.text;
    const std::array<Jass::JassSource, 2> sources{{{"common.j", fixture.common}, {"war3map.j", translated.text}}};
    Jass::JassCheckOptions checks; checks.threads = 1;
    return Jass::CheckJass(sources, checks);
}

void TestCatalog(const Fixture& fixture, const UiCatalog& catalog, const std::vector<Diagnostic>& loadDiagnostics) {
    const auto& index = catalog.CallbackActions();
    Require(index.complete && index.actions.size() == 1, "only ProbeModuleCreate is a callback block");
    const auto* module = index.Find("ProbeModuleCreate");
    Require(module != nullptr && module->codeArgument == 2 && module->source.filename() == "module.j",
        "declaration found below the legacy jass root through its real file");
    Require(index.Find("ProbePlain") == nullptr && index.Find("ProbeCodeFirst") == nullptr && index.Find("ProbeAmbiguous") == nullptr,
        "same-count, code-first and inconsistent declarations are ordinary actions");
    Require(HasCode(loadDiagnostics, "ui.callback.ambiguous"), "inconsistent declarations are reported: " + Details(loadDiagnostics));

    // 现代布局：<ui>/<层>/jass 下的声明同样参与，规则不依赖函数名或某个函数库。
    const auto modern = fixture.tool.parent_path() / "modern/ui";
    WriteText(modern / "config", "layer\n");
    WriteText(modern / "layer/action.txt", "[ModernCallback]\n");
    WriteText(modern / "layer/jass/deep/api.j", "function ModernCallback takes code run returns nothing\nendfunction\n");
    const auto modernCatalog = UiCatalog::Load(modern.parent_path());
    Require(modernCatalog.Succeeded() && modernCatalog.value->CallbackActions().Find("ModernCallback") != nullptr,
        "modern layer jass directory is scanned");
}

void Run(const std::filesystem::path& root) {
    TestScanner();
    const auto fixture = MakeFixture(root / "valid");
    const auto catalog = UiCatalog::Load(fixture.ui);
    Require(catalog.Succeeded(), "original callback catalog: " + Details(catalog.diagnostics));
    TestCatalog(fixture, *catalog.value, catalog.diagnostics);

    const auto record = [](const char* value) { return Child(Action("RecordInteger", {Scalar(3, value)})); };
    auto document = MakeDocument(*catalog.value, {
        Action("ProbeModuleCreate", {Scalar(3, "1"), Scalar(3, "2")}, {record("7"), record("8")}),
        Action("ProbeModuleCreate", {Scalar(3, "3"), Scalar(3, "4")}),
        Action("ProbeModuleCreate", {Scalar(3, "5"), Scalar(3, "6")}, {Child(Action("RecordInteger", {Scalar(3, "9")}, {}, false))}),
    });
    MapScriptInputs inputs; inputs.catalog = &*catalog.value; inputs.triggers = &document;
    const auto generated = GenerateMapScript(inputs);
    Require(generated.Succeeded(), "callback generation: " + Details(generated.diagnostics));
    const auto& text = generated.text;
    Require(Count(text, "function Trig_CallbackFunc001A takes nothing returns nothing") == 1,
        "callback body becomes one independent function");
    Require(Count(text, "call ProbeModuleCreate(1, 2, function Trig_CallbackFunc001A)") == 1,
        "block function is passed as the trailing code argument");
    Require(Count(text, "call ProbeModuleCreate(3, 4, null)") == 1 && Count(text, "call ProbeModuleCreate(5, 6, null)") == 1,
        "empty or fully disabled block passes null");
    Require(Count(text, "Trig_CallbackFunc002A") == 0 && Count(text, "Trig_CallbackFunc003A") == 0 && Count(text, "RecordInteger(9)") == 0,
        "no function is emitted for empty blocks and disabled children are not generated");
    const auto body = text.find("function Trig_CallbackFunc001A");
    const auto end = text.find("endfunction", body);
    Require(body != std::string::npos && text.find("call RecordInteger(7)", body) < end && text.find("call RecordInteger(8)", body) < end,
        "both child actions live inside the callback function in order");
    const auto actions = text.find("function Trig_CallbackActions");
    Require(actions != std::string::npos && body < actions && text.find("RecordInteger(7)", actions) == std::string::npos,
        "callback function precedes the trigger actions and children are not inlined into them");

    std::string translated;
    const auto checked = RunPipeline(text, fixture, translated);
    Require(checked.Succeeded() && checked.errorsIgnored == 0, "actual strict final JASS check: " + Details(checked.diagnostics));
    Require(Count(translated, "function ProbeModuleCreate takes integer parentKey,integer childKey,code callback returns nothing") +
        Count(translated, "function ProbeModuleCreate takes integer parentKey, integer childKey, code callback returns nothing") == 1,
        "the real three-parameter declaration comes from the included library");

    // 普通动作下的子动作不能静默丢弃。
    auto plain = MakeDocument(*catalog.value, {Action("ProbePlain", {Scalar(3, "1")}, {record("2")})});
    MapScriptInputs plainInputs; plainInputs.catalog = &*catalog.value; plainInputs.triggers = &plain;
    const auto refused = GenerateMapScript(plainInputs);
    Require(!refused.Succeeded() && HasCode(refused.diagnostics, "TRIGGER_CHILDREN_UNSUPPORTED"),
        "children under a non-callback action are an explicit error");
}
} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / ("war3-callback-action-tests-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        Require(std::filesystem::create_directory(root), "test root must be a new owned directory");
        Run(root);
        std::filesystem::remove_all(root);
        std::cout << "CallbackAction: scan, legacy/modern recognition, generation, strict check and refusal passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << "\nOriginal test fixtures retained at " << root.string() << '\n';
        return 1;
    }
}
