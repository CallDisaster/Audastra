#include "War3MapCore/Jass/JassChecker.h"
#include "War3MapCore/Jass/Preprocessor.h"
#include "War3MapCore/Jass/VJass.h"
#include "War3MapCore/Script/MapScript.h"
#include "War3MapCore/Script/ScriptFrameworkInputs.h"
#include "War3MapCore/Script/ScriptPipeline.h"
#include "War3MapCore/Trigger/TriggerBlocks.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Trigger/TriggerGraphEditing.h"
#include "War3MapCore/Trigger/UiCallbackActions.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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
    WriteText(result.ui / "probe/define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer,integer\n"
        "typename=1,0,0,Type,string\nstring=0,1,1,String,string\nscriptcode=1,0,0,Code,string\nNull=0,0,0,Value\nAnyReturnType=0,0,0,Any\n"
        "frame=0,1,1,Frame,integer\nprobegroup=0,1,1,Group,handle\nplayer=0,1,1,Player,player\nboolean=0,1,1,Boolean,boolean\n"
        "unit=0,1,1,Unit,unit\nreal=0,1,1,Real,real\ndegree=0,1,1,Degree,real\n"
        "[TriggerParams]\ntypename01_integer=0,typename,1,Integer\ntypename02_real=0,typename,2,Real\ntypename38_degree=0,typename,38,Degree\ntypename39_frame=0,typename,39,Frame\ntypename91_probegroup=0,typename,91,Group\nProbeTop =0,integer,1,Top\nPlayer00=0,player,Player(0),Player 1\nProbeNoCheck=0,boolean,false,No check\n"
        "typename03_boolean=0,typename,3,Boolean\ntypename04_string=0,typename,4,String\ntypename07_unit=0,typename,7,Unit\n");
    WriteText(result.ui / "probe/call.txt",
        "[YDWEGetAnyTypeLocalVariable]\nreturns=AnyReturnType\n[[.args]]\ntype=typename\n[[.args]]\ntype=string\n"
        "[KKGetHandleId]\nreturns=integer\n[[.args]]\ntype=typename\n[[.args]]\ntype=Null\n[ProbeUnit]\nreturns=unit\n"
        "[KKWEGetObjectTableData]\nreturns=AnyReturnType\n[[.args]]\ntype=typename\n[[.args]]\ntype=Null\n"
        "[[.args]]\ntype=typename\n[[.args]]\ntype=Null\n");
    WriteText(result.ui / "probe/action.txt",
        "[ProbeModuleCreate]\ntitle=\"Probe - create module\"\n[[.args]]\ntype=integer\n[[.args]]\ntype=integer\n"
        "[ProbeEnum]\ntitle=\"Probe - 选取探针做动作 (多项)\"\n[[.args]]\ntype=integer\n"
        // 局部变量名在 YDWE 中是代码参数；公共 T 图批次不接受代码参数，合成库改用字符串，生成时同样取原值。
        "[YDWESetAnyTypeLocalVariable]\n[[.args]]\ntype=typename\n[[.args]]\ntype=string\n[[.args]]\ntype=Null\n"
        "[RecordInteger]\n[[.args]]\ntype=integer\n"
        "[KKWESetObjectTableData]\n[[.args]]\ntype=typename\n[[.args]]\ntype=Null\n[[.args]]\ntype=typename\n[[.args]]\ntype=Null\n"
        "[[.args]]\ntype=typename\n[[.args]]\ntype=Null\n"
        "[KKWEFlushObjectTableData]\n[[.args]]\ntype=typename\n[[.args]]\ntype=Null\n[[.args]]\ntype=typename\n[[.args]]\ntype=Null\n"
        "[[.args]]\ntype=typename\n"
        "[DzFrameSetScriptMultiple]\n[[.args]]\ntype=player\n[[.args]]\ntype=integer\n[[.args]]\ntype=integer\n"
        "[DzTriggerRegisterKeyEventMultiple]\n[[.args]]\ntype=player\n[[.args]]\ntype=integer\n[[.args]]\ntype=integer\n"
        "[YDWEExecuteTriggerMultiple]\n[[.args]]\ntype=integer\n[[.args]]\ntype=boolean\n"
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
    WriteText(result.libraries / "ProbeModule.cfg", "ProbeModuleCreate\nProbeEnum\n");
    WriteText(result.libraries / "ProbeModule.j", "#include \"addon/module.j\"\n");
    WriteText(result.libraries / "addon/module.j", "#ifndef PROBE_MODULE_DECLARED\n#define PROBE_MODULE_DECLARED\n"
        "library ProbeModuleLibrary\n"
        "    function ProbeModuleCreate takes integer parentKey, integer childKey, code callback returns nothing\n"
        "        call RecordInteger(parentKey + childKey)\n"
        "    endfunction\n"
        "    function ProbeEnum takes integer value, code callback returns nothing\n"
        "        call RecordInteger(value)\n"
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
std::shared_ptr<GraphEcaValue> Branch(int branch, std::shared_ptr<GraphEcaValue> node) { node->childBranch = branch; return node; }

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
    Require(index.complete && index.actions.size() == 2, "only ProbeModuleCreate and ProbeEnum are callback blocks");
    const auto* module = index.Find("ProbeModuleCreate");
    Require(module != nullptr && module->codeArgument == 2 && module->source.filename() == "module.j" && !module->sharesLocals,
        "declaration found below the legacy jass root through its real file; a deferred callback owns its locals");
    const auto* enumeration = index.Find("ProbeEnum");
    Require(enumeration != nullptr && enumeration->codeArgument == 1 && enumeration->sharesLocals,
        "a title with 做动作 marks an immediate enumeration that shares outer locals");
    Require(index.Find("ProbePlain") == nullptr && index.Find("ProbeCodeFirst") == nullptr && index.Find("ProbeAmbiguous") == nullptr,
        "same-count, code-first and inconsistent declarations are ordinary actions");
    Require(HasCode(index.diagnostics, "ui.callback.ambiguous") && !HasCode(loadDiagnostics, "ui.callback.ambiguous"),
        "inconsistent declarations are reported with the lazily built index, not as catalog load noise: " + Details(index.diagnostics));
    // 同一次载入的拷贝共用同一份识别结果。
    const UiCatalog copy = catalog;
    Require(&copy.CallbackActions() == &index, "catalog copies share one callback index");

    // 现代布局：<ui>/<层>/jass 下的声明同样参与，规则不依赖函数名或某个函数库。
    const auto modern = fixture.tool.parent_path() / "modern/ui";
    WriteText(modern / "config", "layer\n");
    WriteText(modern / "layer/action.txt", "[ModernCallback]\n");
    WriteText(modern / "layer/jass/deep/api.j", "function ModernCallback takes code run returns nothing\nendfunction\n");
    const auto modernCatalog = UiCatalog::Load(modern.parent_path());
    Require(modernCatalog.Succeeded() && modernCatalog.value->CallbackActions().Find("ModernCallback") != nullptr,
        "modern layer jass directory is scanned");
}

std::string HashKey(std::string_view text) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%08X", StormStringHash(text));
    return buffer;
}

// KKWE 生成器自己展开的条目：模块（创建 / 运行 / 设置返回值）、真局部变量、选取单位到单位组（多动作）、跳出循环。
// 原创合成函数库；名称用 KKWE 的定义名，因为生成规则按名称识别。只验证生成文本与严格检查，不代表游戏运行。
void TestKkweGeneratedEntries(const std::filesystem::path& root) {
    const auto tool = root / "kkwe";
    const auto ui = tool / "share/mpq";
    WriteText(ui / "config", "probe\n");
    WriteText(ui / "probe/define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer,integer\nreal=0,1,1,Real,real\n"
        "string=0,1,1,String,string\nunit=0,1,1,Unit,unit\ngroup=0,1,1,Group,group\nfilter=0,1,1,Filter,boolexpr\n"
        "typename=1,0,0,Type,string\nNull=0,0,0,Value\nAnyReturnType=0,0,0,Any\n"
        "[TriggerParams]\ntypename01_integer=0,typename,1,Integer\ntypename02_real=0,typename,2,Real\n"
        "typename07_unit=0,typename,7,Unit\nProbeNoFilter=0,filter,null,No filter\n");
    // 变量名在 KKWE 中是代码参数；公共 T 图批次不接受代码参数，合成库改用字符串，生成时同样取原值。
    WriteText(ui / "probe/action.txt",
        "[MHModule_Create]\ntitle=\"Probe - create module\"\n[[.args]]\ntype=integer\n[[.args]]\ntype=integer\n"
        "[MHModule_Evaluate]\n[[.args]]\ntype=integer\n[[.args]]\ntype=integer\n"
        "[MHModule_SetRetval]\n[[.args]]\ntype=typename\n[[.args]]\ntype=string\n[[.args]]\ntype=Null\n"
        "[MHModule_SetArrayRetval]\n[[.args]]\ntype=typename\n[[.args]]\ntype=string\n[[.args]]\ntype=integer\n[[.args]]\ntype=Null\n"
        "[YDWESetAnyTypeLocalVariable]\n[[.args]]\ntype=typename\n[[.args]]\ntype=string\n[[.args]]\ntype=Null\n"
        "[KKAPISetLocalVariables]\n[[.args]]\ntype=typename\n[[.args]]\ntype=string\n[[.args]]\ntype=Null\n"
        "[KKAPISetAnyTypeLocalArray]\n[[.args]]\ntype=typename\n[[.args]]\ntype=string\n[[.args]]\ntype=integer\n[[.args]]\ntype=Null\n"
        "[KKAPIEnumUnitsInRangeMultiple]\n[[.args]]\ntype=group\n[[.args]]\ntype=real\n[[.args]]\ntype=real\n[[.args]]\ntype=real\n"
        "[[.args]]\ntype=filter\n"
        "[YDWEExitLoop]\n[RecordReal]\n[[.args]]\ntype=real\n[ProbeKill]\n[[.args]]\ntype=unit\n");
    WriteText(ui / "probe/call.txt",
        "[YDWEGetAnyTypeLocalVariable]\nreturns=AnyReturnType\n[[.args]]\ntype=string\n"
        "[KKAPIGetLocalVariables]\nreturns=AnyReturnType\n[[.args]]\ntype=string\n"
        "[KKAPIGetAnyTypeLocalArray]\nreturns=AnyReturnType\n[[.args]]\ntype=string\n[[.args]]\ntype=integer\n"
        "[ProbeGroup]\nreturns=group\n[ProbeHero]\nreturns=unit\n[GetEnumUnit]\nreturns=unit\n");
    const std::string common = "type agent extends handle\ntype widget extends agent\ntype unit extends widget\n"
        "type group extends agent\ntype trigger extends agent\ntype triggeraction extends handle\ntype hashtable extends agent\n"
        "type boolexpr extends agent\n"
        "native CreateTrigger takes nothing returns trigger\n"
        "native TriggerAddAction takes trigger t, code action returns triggeraction\n"
        "native GetTriggeringTrigger takes nothing returns trigger\nnative GetHandleId takes handle h returns integer\n"
        "native StringHash takes string s returns integer\n"
        "native SaveInteger takes hashtable t, integer p, integer c, integer v returns nothing\n"
        "native LoadInteger takes hashtable t, integer p, integer c returns integer\n"
        "native SaveReal takes hashtable t, integer p, integer c, real v returns nothing\n"
        "native LoadReal takes hashtable t, integer p, integer c returns real\n"
        "native FlushChildHashtable takes hashtable t, integer p returns nothing\n"
        "native GroupEnumUnitsInRange takes group g, real x, real y, real r, boolexpr f returns nothing\n"
        "native ForGroup takes group g, code c returns nothing\nnative GetEnumUnit takes nothing returns unit\n"
        "native MHModule_Create takes integer a, integer b, code c returns nothing\n"
        "native MHModule_Evaluate takes integer a, integer b returns boolean\n"
        "native MHModule_GetCode takes integer a, integer b returns code\n"
        "native MHGame_GetCodeName takes code c returns string\n"
        "native RecordReal takes real v returns nothing\nnative ProbeKill takes unit u returns nothing\n"
        "native ProbeGroup takes nothing returns group\nnative ProbeHero takes nothing returns unit\n";
    // 回调块识别读函数库 J 的声明：模块函数多一个末尾 code 参数。
    WriteText(tool / "jass/module.j", common);
    const auto catalog = UiCatalog::Load(ui);
    Require(catalog.Succeeded(), "KKWE probe catalog: " + Details(catalog.diagnostics));
    Require(catalog.value->CallbackActions().Find("MHModule_Create") != nullptr, "module creation is a callback block");

    const auto callValue = [](std::string function, std::vector<GraphArgumentValue> args) {
        GraphArgumentValue result;
        auto call = std::make_shared<GraphEcaValue>();
        call->function = std::move(function); call->kind = UiDefinitionKind::Call; call->arguments = std::move(args);
        result.call = call;
        return result;
    };
    // 与 YDWE 函数库相同：读取局部变量只有变量名参数，类型取自所在参数位。
    const auto readLocal = [&](const char* name) {
        return callValue("YDWEGetAnyTypeLocalVariable", {Scalar(3, name)});
    };
    auto document = MakeDocument(*catalog.value, {
        Action("MHModule_Create", {Scalar(3, "1"), Scalar(3, "2")}, {
            Child(Action("YDWESetAnyTypeLocalVariable", {Scalar(0, "typename02_real"), Scalar(3, "Damage"), Scalar(3, "5.0")})),
            Child(Action("MHModule_SetRetval", {Scalar(0, "typename02_real"), Scalar(3, "Damage"), readLocal("Damage")})),
            Child(Action("MHModule_SetArrayRetval", {Scalar(0, "typename01_integer"), Scalar(3, "Hits"), Scalar(3, "3"), Scalar(3, "7")})),
        }),
        Action("YDWESetAnyTypeLocalVariable", {Scalar(0, "typename02_real"), Scalar(3, "Damage"), Scalar(3, "1.0")}),
        Action("MHModule_Evaluate", {Scalar(3, "1"), Scalar(3, "2")}, {
            Child(Action("YDWESetAnyTypeLocalVariable", {Scalar(0, "typename01_integer"), Scalar(3, "Arg"), Scalar(3, "9")})),
        }),
        Action("RecordReal", {readLocal("Damage")}),
        Action("MHModule_SetRetval", {Scalar(0, "typename02_real"), Scalar(3, "Lost"), Scalar(3, "1.0")}),
        Action("KKAPISetLocalVariables", {Scalar(0, "typename07_unit"), Scalar(3, "Hero"), callValue("ProbeHero", {})}),
        Action("KKAPISetAnyTypeLocalArray", {Scalar(0, "typename02_real"), Scalar(3, "Scale"), Scalar(3, "2"), Scalar(3, "1.5")}),
        Action("RecordReal", {callValue("KKAPIGetAnyTypeLocalArray", {Scalar(3, "Scale"), Scalar(3, "2")})}),
        Action("ProbeKill", {callValue("KKAPIGetLocalVariables", {Scalar(3, "Hero")})}),
        Action("KKAPIEnumUnitsInRangeMultiple", {callValue("ProbeGroup", {}), Scalar(3, "0"), Scalar(3, "0"), Scalar(3, "512"),
            Scalar(0, "ProbeNoFilter")}, {Child(Action("ProbeKill", {callValue("GetEnumUnit", {})}))}),
    });
    MapScriptInputs inputs; inputs.catalog = &*catalog.value; inputs.triggers = &document;
    const auto generated = GenerateMapScript(inputs);
    Require(generated.Succeeded(), "KKWE entry generation: " + Details(generated.diagnostics));
    const auto& text = generated.text;
    const auto bodyOf = [&](std::string_view function) {
        const auto begin = text.find("function " + std::string(function) + " takes");
        Require(begin != std::string::npos, "missing " + std::string(function) + ":\n" + text);
        return text.substr(begin, text.find("endfunction", begin) - begin);
    };
    const std::string current = "LoadInteger(YDLOC, " + HashKey("Trig_CallbackFunc001A") + ", " + HashKey("Current") + ")";
    const std::string parent = "LoadInteger(YDLOC, " + HashKey("Trig_CallbackFunc001A") + ", " + HashKey("Parent") + ")";
    const auto module = bodyOf("Trig_CallbackFunc001A");
    Require(module.find("ydl_localvar_step") == std::string::npos
        && module.find("call SaveReal(YDLOC, " + current + ", " + HashKey("Damage") + ", 5.0)") != std::string::npos,
        "module locals live on the module's current page without a frame of their own:\n" + module);
    Require(module.find("call SaveReal(YDLOC, " + parent + ", " + HashKey("Damage") + ", LoadReal(YDLOC, " + current + ", "
            + HashKey("Damage") + "))") != std::string::npos
        && module.find("call SaveInteger(YDLOC, " + parent + ", " + HashKey("Hits") + " + (3), 7)") != std::string::npos,
        "return values are written to the caller's page, arrays by key + index:\n" + module);
    Require(!HasCode(generated.diagnostics, "callback-local-scope"), "module locals are not reported as lost: "
        + Details(generated.diagnostics));
    const auto main = bodyOf("Trig_CallbackActions");
    const std::string page = "StringHash(MHGame_GetCodeName(MHModule_GetCode(1, 2)))";
    const auto parentSave = main.find("call SaveInteger(YDLOC, " + page + ", " + HashKey("Parent")
        + ", GetHandleId(GetTriggeringTrigger())*ydl_localvar_step)");
    const auto currentSave = main.find("call SaveInteger(YDLOC, " + page + ", " + HashKey("Current") + ", " + page + ")");
    const auto argument = main.find("call SaveInteger(YDLOC, " + page + ", " + HashKey("Arg") + ", 9)");
    const auto evaluate = main.find("call MHModule_Evaluate(1, 2)");
    const auto flush = main.find("call FlushChildHashtable(YDLOC, " + page + ")");
    Require(main.find("local integer ydl_localvar_step") != std::string::npos && parentSave != std::string::npos
        && currentSave != std::string::npos && argument != std::string::npos && evaluate != std::string::npos
        && flush != std::string::npos && parentSave < currentSave && currentSave < argument && argument < evaluate && evaluate < flush,
        "evaluating a module passes this frame as its parent, writes parameters to the module page and clears it after:\n" + main);
    Require(main.find("call RecordReal(LoadReal(YDLOC, GetHandleId(GetTriggeringTrigger())*ydl_localvar_step, "
            + HashKey("Damage") + "))") != std::string::npos,
        "the caller reads the returned value as its own local:\n" + main);
    Require(HasCode(generated.diagnostics, "module-retval-outside") && text.find(HashKey("Lost")) == std::string::npos,
        "a return value outside any module is reported and not generated: " + Details(generated.diagnostics));
    Require(main.find("local unit Hero") != std::string::npos && main.find("local real array Scale") != std::string::npos
        && main.find("set Hero = ProbeHero()") != std::string::npos && main.find("set Scale[2] = 1.5") != std::string::npos
        && main.find("call RecordReal(Scale[2])") != std::string::npos && main.find("call ProbeKill(Hero)") != std::string::npos
        && main.find("set Hero = null") != std::string::npos,
        "true locals are plain JASS locals, handles cleared at the end:\n" + main);
    Require(main.find("local group KKWE_Group") != std::string::npos && main.find("set KKWE_Group = ProbeGroup()") != std::string::npos
        && main.find("call GroupEnumUnitsInRange(KKWE_Group, 0, 0, 512, null)") != std::string::npos
        && main.find("call ForGroup(KKWE_Group, function Trig_CallbackFunc010A)") != std::string::npos
        && main.find("set KKWE_Group = null") != std::string::npos
        && bodyOf("Trig_CallbackFunc010A").find("call ProbeKill(GetEnumUnit())") != std::string::npos,
        "the enumeration block fills the group and runs its actions for each unit:\n" + text);
    Require(text.find("MHModule_SetRetval") == std::string::npos && text.find("MHModule_SetArrayRetval") == std::string::npos
        && text.find("KKAPI") == std::string::npos,
        "no generated entry is written as an undeclared call:\n" + text);

    // 严格检查生成的触发器函数（与原始错误同类：保存前的语法与类型检查）。
    const auto first = text.find("function Trig_Callback");
    const auto last = text.find("function InitCustomTriggers");
    Require(first != std::string::npos && last != std::string::npos && first < last, "trigger section:\n" + text);
    const std::string checked = "globals\n    hashtable YDLOC = null\n    trigger gg_trg_Callback = null\nendglobals\n"
        + text.substr(first, text.rfind("//====", last) - first);
    const std::array<Jass::JassSource, 2> sources{{{"common.j", common}, {"war3map.j", checked}}};
    Jass::JassCheckOptions checks; checks.threads = 1;
    const auto result = Jass::CheckJass(sources, checks);
    Require(result.Succeeded() && result.errorsIgnored == 0, "strict check of the generated triggers: " + Details(result.diagnostics)
        + "\n" + checked);

    // YDWE“跳出循环”：写成 exitwhen true，不是一个不存在的函数调用。
    auto exit = MakeDocument(*catalog.value, {Action("YDWEExitLoop")});
    MapScriptInputs exitInputs; exitInputs.catalog = &*catalog.value; exitInputs.triggers = &exit;
    const auto exitScript = GenerateMapScript(exitInputs);
    Require(exitScript.Succeeded() && exitScript.text.find("exitwhen true") != std::string::npos
        && exitScript.text.find("YDWEExitLoop") == std::string::npos, "exit loop:\n" + exitScript.text);
}

// KKWE 运动器：应用到单位 / 特效、设置刷新回调、删除运动器、清除当前运动器。原创合成函数库，名称用 KKWE 的定义名。
// 子块 0 是参数区，“应用到…”的 1、2 块是刷新、到达回调；回调里的局部变量在 StringHash("Mover") + 当前回调的运动器 页上。
void TestKkweMoverEntries(const std::filesystem::path& root) {
    const auto tool = root / "kkwe-mover";
    const auto ui = tool / "share/mpq";
    WriteText(ui / "config", "probe\n");
    WriteText(ui / "probe/define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer,integer\nreal=0,1,1,Real,real\n"
        "string=0,1,1,String,string\nunit=0,1,1,Unit,unit\neffect=0,1,1,Effect,effect\n"
        "typename=1,0,0,Type,string\nNull=0,0,0,Value\nAnyReturnType=0,0,0,Any\n"
        "[TriggerParams]\ntypename02_real=0,typename,2,Real\n");
    WriteText(ui / "probe/action.txt",
        "[MHMover_ApplyUnit]\nscript_name = MHMover_ApplyWidget\n[[.args]]\ntype=integer\n[[.args]]\ntype=unit\n"
        "[MHMover_ApplyEffect]\n[[.args]]\ntype=integer\n[[.args]]\ntype=effect\n"
        "[MHMover_SetOnUpdate]\n[[.args]]\ntype=integer\n"
        "[MHMover_Remove]\n[[.args]]\ntype=integer\n[MHMover_RemoveTriggerMover]\n"
        "[YDWESetAnyTypeLocalVariable]\n[[.args]]\ntype=typename\n[[.args]]\ntype=string\n[[.args]]\ntype=Null\n"
        "[RecordReal]\n[[.args]]\ntype=real\n");
    WriteText(ui / "probe/call.txt",
        "[YDWEGetAnyTypeLocalVariable]\nreturns=AnyReturnType\n[[.args]]\ntype=string\n"
        "[MHMover_CreateParabola]\nreturns=integer\n[ProbeMover]\nreturns=integer\n[ProbeHero]\nreturns=unit\n"
        "[ProbeEffect]\nreturns=effect\n");
    const std::string common = "type agent extends handle\ntype widget extends agent\ntype unit extends widget\n"
        "type effect extends agent\ntype trigger extends agent\ntype triggeraction extends handle\ntype hashtable extends agent\n"
        "native CreateTrigger takes nothing returns trigger\n"
        "native TriggerAddAction takes trigger t, code action returns triggeraction\n"
        "native GetTriggeringTrigger takes nothing returns trigger\nnative GetHandleId takes handle h returns integer\n"
        "native SaveInteger takes hashtable t, integer p, integer c, integer v returns nothing\n"
        "native LoadInteger takes hashtable t, integer p, integer c returns integer\n"
        "native SaveReal takes hashtable t, integer p, integer c, real v returns nothing\n"
        "native LoadReal takes hashtable t, integer p, integer c returns real\n"
        "native FlushChildHashtable takes hashtable t, integer p returns nothing\n"
        "native MHMover_ApplyWidget takes integer id, widget subject, code on_update, code on_reach returns boolean\n"
        "native MHMover_ApplyEffect takes integer id, effect subject, code on_update, code on_reach returns boolean\n"
        "native MHMover_SetOnUpdate takes integer id, code callback returns boolean\n"
        "native MHMover_Remove takes integer id returns boolean\nnative MHMover_GetTriggerMover takes nothing returns integer\n"
        "native MHMover_CreateParabola takes nothing returns integer\nnative ProbeMover takes nothing returns integer\n"
        "native ProbeHero takes nothing returns unit\nnative ProbeEffect takes nothing returns effect\n"
        "native RecordReal takes real v returns nothing\n";
    // 与 KKWE 函数库相同：设置刷新回调的声明多一个末尾 code 参数，会被识别成回调块，但必须按运动器生成。
    WriteText(tool / "jass/mover.j", common);
    const auto catalog = UiCatalog::Load(ui);
    Require(catalog.Succeeded(), "KKWE mover catalog: " + Details(catalog.diagnostics));
    const auto* apply = FindTriggerBlockBranches("MHMover_ApplyUnit", &*catalog.value);
    const auto* onUpdate = FindTriggerBlockBranches("MHMover_SetOnUpdate", &*catalog.value);
    Require(apply != nullptr && apply->size() == 3 && std::string_view((*apply)[1].label) == "刷新"
        && std::string_view((*apply)[2].label) == "到达" && onUpdate != nullptr && onUpdate->size() == 2
        && std::string_view((*onUpdate)[0].key) == "params" && std::string_view((*onUpdate)[1].key) == "actions",
        "mover blocks follow KKWE: parameters, then update / reach (apply) or actions (set callback)");

    const auto callValue = [](std::string function, std::vector<GraphArgumentValue> args) {
        GraphArgumentValue result;
        auto call = std::make_shared<GraphEcaValue>();
        call->function = std::move(function); call->kind = UiDefinitionKind::Call; call->arguments = std::move(args);
        result.call = call;
        return result;
    };
    const auto readLocal = [&](const char* name) { return callValue("YDWEGetAnyTypeLocalVariable", {Scalar(3, name)}); };
    const auto setLocal = [](const char* name, const char* value) {
        return Action("YDWESetAnyTypeLocalVariable", {Scalar(0, "typename02_real"), Scalar(3, name), Scalar(3, value)});
    };
    auto document = MakeDocument(*catalog.value, {
        setLocal("Bonus", "2.0"),
        Action("MHMover_ApplyUnit", {callValue("MHMover_CreateParabola", {}), callValue("ProbeHero", {})}, {
            Branch(0, setLocal("Speed", "3.0")),
            Branch(1, Action("RecordReal", {readLocal("Speed")})),
            Branch(1, Action("RecordReal", {readLocal("Bonus")})),
            Branch(2, Action("RecordReal", {readLocal("Speed")})),
            Branch(2, Action("MHMover_RemoveTriggerMover")),
        }),
        Action("MHMover_SetOnUpdate", {callValue("ProbeMover", {})}, {Branch(1, Action("RecordReal", {readLocal("Bonus")}))}),
        Action("MHMover_ApplyEffect", {callValue("ProbeMover", {}), callValue("ProbeEffect", {})},
            {Branch(1, Action("RecordReal", {Scalar(3, "1.0")}))}),
        Action("MHMover_Remove", {callValue("ProbeMover", {})}),
        Action("MHMover_RemoveTriggerMover"),
    });
    MapScriptInputs inputs; inputs.catalog = &*catalog.value; inputs.triggers = &document;
    const auto generated = GenerateMapScript(inputs);
    Require(generated.Succeeded(), "KKWE mover generation: " + Details(generated.diagnostics));
    const auto& text = generated.text;
    const auto bodyOf = [&](std::string_view function) {
        const auto begin = text.find("function " + std::string(function) + " takes");
        Require(begin != std::string::npos, "missing " + std::string(function) + ":\n" + text);
        return text.substr(begin, text.find("endfunction", begin) - begin);
    };
    const std::string mover = HashKey("Mover");
    const std::string current = mover + " + MHMover_GetTriggerMover()";
    const auto update = bodyOf("Trig_CallbackFunc002MoverUpdate");
    Require(update.find("call RecordReal(LoadReal(YDLOC, " + current + ", " + HashKey("Speed") + "))") != std::string::npos
        && update.find("call RecordReal(LoadReal(YDLOC, " + current + ", " + HashKey("Bonus") + "))") != std::string::npos
        && update.find("ydl_localvar_step") == std::string::npos,
        "update callback reads locals from the current mover's page:\n" + update);
    const auto reach = bodyOf("Trig_CallbackFunc002MoverReach");
    const auto clear = reach.find("call FlushChildHashtable(YDLOC, " + current + ")");
    Require(reach.find("call RecordReal(LoadReal(YDLOC, " + current + ", " + HashKey("Speed") + "))") != std::string::npos
        && clear != std::string::npos && reach.find("call MHMover_Remove(MHMover_GetTriggerMover())") > clear
        && reach.find("call MHMover_Remove(MHMover_GetTriggerMover())") != std::string::npos,
        "clearing the current mover inside its callback clears its page, then removes it:\n" + reach);
    Require(bodyOf("Trig_CallbackFunc003Mover").find("call RecordReal(LoadReal(YDLOC, " + current + ", " + HashKey("Bonus") + "))")
            != std::string::npos,
        "set-update callback uses the same mover page:\n" + text);

    const auto main = bodyOf("Trig_CallbackActions");
    const std::string param = mover + " + KKWE_Mover";
    const std::string frame = "GetHandleId(GetTriggeringTrigger())*ydl_localvar_step";
    const auto assign = main.find("set KKWE_Mover = MHMover_CreateParabola()");
    const auto speed = main.find("call SaveReal(YDLOC, " + param + ", " + HashKey("Speed") + ", 3.0)");
    const auto bonus = main.find("call SaveReal(YDLOC, " + param + ", " + HashKey("Bonus") + ", LoadReal(YDLOC, " + frame + ", "
        + HashKey("Bonus") + "))");
    const auto applied = main.find("call MHMover_ApplyWidget(KKWE_Mover, ProbeHero(), function Trig_CallbackFunc002MoverUpdate, "
        "function Trig_CallbackFunc002MoverReach)");
    Require(main.find("local integer KKWE_Mover") != std::string::npos && assign != std::string::npos && speed != std::string::npos
        && bonus != std::string::npos && applied != std::string::npos && assign < speed && speed < bonus && bonus < applied
        && main.find(param + ", " + HashKey("Speed") + ", LoadReal") == std::string::npos,
        "apply evaluates the mover once, writes parameters and the captured outer local to its page, then passes both callbacks:\n" + main);
    Require(main.find("call MHMover_SetOnUpdate(KKWE_Mover, function Trig_CallbackFunc003Mover)") != std::string::npos
        && main.find("call MHMover_ApplyEffect(KKWE_Mover, ProbeEffect(), function Trig_CallbackFunc004MoverUpdate, "
            "function Trig_CallbackFunc004MoverReach)") != std::string::npos,
        "set-update and apply-to-effect pass their generated callbacks:\n" + main);
    const auto removeClear = main.find("call FlushChildHashtable(YDLOC, " + param + ")");
    Require(removeClear != std::string::npos && main.find("call MHMover_Remove(KKWE_Mover)") > removeClear
        && main.find("call MHMover_Remove(KKWE_Mover)") != std::string::npos,
        "removing a mover also clears its page:\n" + main);
    Require(HasCode(generated.diagnostics, "mover-remove-outside") && !HasCode(generated.diagnostics, "callback-local-scope")
        && text.find("MHMover_RemoveTriggerMover") == std::string::npos && text.find("MHMover_ApplyUnit") == std::string::npos
        && text.find("Trig_CallbackFunc003A") == std::string::npos,
        "no entry is written as an undeclared call and the set-update block is not a generic callback: "
        + Details(generated.diagnostics) + "\n" + text);

    const auto first = text.find("function Trig_Callback");
    const auto last = text.find("function InitCustomTriggers");
    Require(first != std::string::npos && last != std::string::npos && first < last, "trigger section:\n" + text);
    const std::string checked = "globals\n    hashtable YDLOC = null\n    trigger gg_trg_Callback = null\nendglobals\n"
        + text.substr(first, text.rfind("//====", last) - first);
    const std::array<Jass::JassSource, 2> sources{{{"common.j", common}, {"war3map.j", checked}}};
    Jass::JassCheckOptions checks; checks.threads = 1;
    const auto result = Jass::CheckJass(sources, checks);
    Require(result.Succeeded() && result.errorsIgnored == 0, "strict check of the generated mover triggers: " + Details(result.diagnostics)
        + "\n" + checked);
}

void Run(const std::filesystem::path& root) {
    TestScanner();
    TestKkweGeneratedEntries(root);
    TestKkweMoverEntries(root);
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

    // 函数库里没有 .cfg 的文件不会自动注入；最终检查报未声明时提示作者 #include 哪个文件。
    // 声明在子目录文件里、由根目录入口文件包含；文档目录里的同名副本无人包含，不能被提示。
    WriteText(fixture.libraries / "extra/probe_extra.j", "function ProbeExtraOne takes nothing returns nothing\nendfunction\n"
        "function ProbeExtraTwo takes nothing returns nothing\nendfunction\n");
    WriteText(fixture.libraries / "extra/doc/function.j", "function ProbeExtraOne takes nothing returns nothing\nendfunction\n");
    WriteText(fixture.libraries / "extra_base.j", "// original empty base\n");
    WriteText(fixture.libraries / "extra_entry.j", "#include \"extra_base.j\"\n  #  include \"extra/probe_extra.j\"\n");
    const std::array<Jass::JassSource, 2> undeclaredSources{{{"common.j", fixture.common},
        {"war3map.j", "function main takes nothing returns nothing\ncall ProbeExtraOne()\ncall ProbeExtraTwo()\ncall ProbeNowhere()\nendfunction\n"}}};
    Jass::JassCheckOptions undeclaredOptions; undeclaredOptions.threads = 1;
    auto undeclared = Jass::CheckJass(undeclaredSources, undeclaredOptions).diagnostics;
    const auto before = undeclared.size();
    AppendMissingIncludeHints(undeclared, 0, {fixture.libraries});
    Require(undeclared.size() == before + 1 && undeclared.back().code == "JASS_MISSING_INCLUDE_HINT"
        && undeclared.back().severity == DiagnosticSeverity::Warning
        && undeclared.back().message.find("#include \"extra_entry.j\"") != std::string::npos
        && undeclared.back().message.find("ProbeExtraOne") != std::string::npos
        && undeclared.back().message.find("ProbeExtraTwo") != std::string::npos
        && undeclared.back().message.find("ProbeNowhere") == std::string::npos,
        "one include hint names the declaring library file and only its functions: " + Details(undeclared));
    // 可选：本机真实 kkwe 函数库（只读）。MemHack 两组函数应分别指向各自的入口文件，不指向 api/ 或文档副本。
#pragma warning(suppress : 4996) // 只读取一次测试开关，不保存指针。
    if (const auto* real = std::getenv("AUDASTRA_KKWE_JASS_DIR"); real != nullptr && *real != '\0') {
        std::vector<Diagnostic> diagnostics{
            {DiagnosticSeverity::Error, "Jass.Check", "JASS_UNDECLARED", "war3map.j:9: 未声明的函数 “MHDebug_ShowLeakMessage”", {}},
            {DiagnosticSeverity::Error, "Jass.Check", "JASS_UNDECLARED", "war3map.j:12: 未声明的函数 “MHModule_Create”", {}}};
        AppendMissingIncludeHints(diagnostics, 0, {std::filesystem::path(real)});
        std::cout << Details(diagnostics);
        Require(Count(Details(diagnostics), "JASS_MISSING_INCLUDE_HINT") == 2
            && Details(diagnostics).find("#include \"memory_hack.j\"") != std::string::npos
            && Details(diagnostics).find("#include \"memory_hack_addon.j\"") != std::string::npos,
            "real kkwe MemHack hints name the two entry files");
    }
    Require(Count(translated, "function ProbeModuleCreate takes integer parentKey,integer childKey,code callback returns nothing") +
        Count(translated, "function ProbeModuleCreate takes integer parentKey, integer childKey, code callback returns nothing") == 1,
        "the real three-parameter declaration comes from the included library");

    // 普通动作下的子节点：与原版生成器一致只写调用本身，但必须逐个警告，不能静默丢弃。
    auto plain = MakeDocument(*catalog.value, {Action("ProbePlain", {Scalar(3, "1")}, {record("2")})});
    MapScriptInputs plainInputs; plainInputs.catalog = &*catalog.value; plainInputs.triggers = &plain;
    const auto ignored = GenerateMapScript(plainInputs);
    Require(ignored.Succeeded() && HasCode(ignored.diagnostics, "children-not-generated"),
        "children under a non-callback action are reported: " + Details(ignored.diagnostics));
    Require(Count(ignored.text, "call ProbePlain(1)") == 1 && Count(ignored.text, "RecordInteger(2)") == 0,
        "the plain call is still emitted and its children are not");

    // 局部变量：遍历类共用外层的一帧（读外层保存的步号），之后执行的回调自建一帧并给出提示。
    const auto local = [](const char* value) {
        return Child(Action("YDWESetAnyTypeLocalVariable", {Scalar(0, "typename01_integer"), Scalar(3, "score"), Scalar(3, value)}));
    };
    auto locals = MakeDocument(*catalog.value, {
        Action("ProbeEnum", {Scalar(3, "1")}, {local("8")}),
        Action("ProbeModuleCreate", {Scalar(3, "1"), Scalar(3, "2")}, {local("9")}),
    });
    MapScriptInputs localInputs; localInputs.catalog = &*catalog.value; localInputs.triggers = &locals;
    const auto scoped = GenerateMapScript(localInputs);
    Require(scoped.Succeeded(), "local generation: " + Details(scoped.diagnostics));
    const auto bodyOf = [&](std::string_view function) {
        const auto begin = scoped.text.find("function " + std::string(function) + " takes");
        Require(begin != std::string::npos, "missing " + std::string(function));
        return scoped.text.substr(begin, scoped.text.find("endfunction", begin) - begin);
    };
    const auto shared = bodyOf("Trig_CallbackFunc001A");
    const auto own = bodyOf("Trig_CallbackFunc002A");
    const auto main = bodyOf("Trig_CallbackActions");
    Require(shared.find("ydl_localvar_step") == std::string::npos && shared.find("0xECE825E7)") != std::string::npos,
        "enumeration callback reads the outer frame's current step instead of opening its own: " + shared);
    Require(main.find("local integer ydl_localvar_step") != std::string::npos,
        "outer actions open the frame because the shared block uses locals: " + main);
    Require(own.find("local integer ydl_localvar_step") != std::string::npos,
        "deferred callback opens its own local frame: " + own);
    std::size_t notes = 0;
    for (const auto& diagnostic : scoped.diagnostics) notes += diagnostic.code == "callback-local-scope" ? 1 : 0;
    Require(notes == 1, "only the deferred callback warns about separate locals: " + Details(scoped.diagnostics));

    // 运行触发器（多项）的参数区：赋值写进被调触发器的页，右边读取的局部变量却属于本函数。
    // 只在参数区读局部变量的触发器（只把参数转交给别的触发器）也要开本次执行的帧，不能读第 2 页没人写过的步号。
    GraphArgumentValue readScore;
    {
        auto call = std::make_shared<GraphEcaValue>();
        call->function = "YDWEGetAnyTypeLocalVariable"; call->kind = UiDefinitionKind::Call;
        call->arguments = {Scalar(0, "typename01_integer"), Scalar(3, "score")};
        readScore.call = call;
    }
    auto forward = MakeDocument(*catalog.value, {Action("YDWEExecuteTriggerMultiple", {Scalar(3, "7"), Scalar(0, "ProbeNoCheck")},
        {Child(Action("YDWESetAnyTypeLocalVariable", {Scalar(0, "typename01_integer"), Scalar(3, "total"), readScore}))})});
    MapScriptInputs forwardInputs; forwardInputs.catalog = &*catalog.value; forwardInputs.triggers = &forward;
    const auto forwarded = GenerateMapScript(forwardInputs);
    Require(forwarded.Succeeded(), "forwarding generation: " + Details(forwarded.diagnostics));
    const auto forwardBegin = forwarded.text.find("function Trig_CallbackActions takes");
    Require(forwardBegin != std::string::npos, "forwarding actions function:\n" + forwarded.text);
    const auto forwardMain = forwarded.text.substr(forwardBegin, forwarded.text.find("endfunction", forwardBegin) - forwardBegin);
    Require(forwardMain.find("local integer ydl_localvar_step") != std::string::npos
        && forwardMain.find("*ydl_localvar_step, 0x") != std::string::npos && forwardMain.find("0xECE825E7)") == std::string::npos,
        "a trigger that only reads locals inside execute parameters opens its own frame and reads it: " + forwardMain);
    Require(forwardMain.find("call SaveInteger(YDLOC, ydl_triggerstep, 0x") != std::string::npos
        && forwardMain.find("call TriggerExecute(ydl_trigger)") != std::string::npos,
        "the parameter is still written to the callee page before executing: " + forwardMain);

    // 函数库扩展的局部变量类型（序号超出内置 38 种）：类型名取自预设，按 TriggerTypes 声明的基础类型存取。
    auto framed = MakeDocument(*catalog.value, {Action("YDWESetAnyTypeLocalVariable",
        {Scalar(0, "typename39_frame"), Scalar(3, "panel"), Scalar(3, "5")})});
    MapScriptInputs frameInputs; frameInputs.catalog = &*catalog.value; frameInputs.triggers = &framed;
    const auto frame = GenerateMapScript(frameInputs);
    Require(frame.Succeeded() && !HasCode(frame.diagnostics, "local-type-missing")
        && !HasCode(frame.diagnostics, "storage-type-unsupported"), "library local type is accepted: " + Details(frame.diagnostics));
    Require(frame.text.find("SaveInteger(") != std::string::npos && frame.text.find(", 5)") != std::string::npos,
        "frame locals are stored as their declared base type integer:\n" + frame.text);
    // 基础类型为 handle 时没有统一的 Save 函数：按 YDHashSet 展开写 YDTRIGGER_<type>(SET)，由函数库头文件定义。
    auto grouped = MakeDocument(*catalog.value, {Action("YDWESetAnyTypeLocalVariable",
        {Scalar(0, "typename91_probegroup"), Scalar(3, "fx"), Scalar(3, "6")})});
    MapScriptInputs groupInputs; groupInputs.catalog = &*catalog.value; groupInputs.triggers = &grouped;
    const auto group = GenerateMapScript(groupInputs);
    Require(group.Succeeded() && !HasCode(group.diagnostics, "local-type-missing")
        && group.text.find("call YDTRIGGER_probegroup(SET)(") != std::string::npos
        && group.text.find("YDTRIGGER_handle") == std::string::npos,
        "library handle types keep their own header macro:\n" + group.text);
    // 实数与角度存进同一个实数槽，混用读到的是同一个值，不报类型冲突；实数与字符串混用仍然要报。
    const auto conflicts = [&](const char* first, const char* second) {
        auto document = MakeDocument(*catalog.value, {
            Action("YDWESetAnyTypeLocalVariable", {Scalar(0, first), Scalar(3, "slot"), Scalar(3, "1.0")}),
            Action("YDWESetAnyTypeLocalVariable", {Scalar(0, second), Scalar(3, "slot"), Scalar(3, "2.0")})});
        MapScriptInputs inputs; inputs.catalog = &*catalog.value; inputs.triggers = &document;
        return HasCode(GenerateMapScript(inputs).diagnostics, "local-type-conflict");
    };
    Require(!conflicts("typename02_real", "typename38_degree"), "real and degree share one stored slot");
    Require(conflicts("typename02_real", "typename04_string"), "real then string still reports the slot conflict");
    // 定义里键名带空格（“ProbeTop =…”）时，触发器存的预设名也带空格，仍按预设值输出。
    auto spaced = MakeDocument(*catalog.value, {Action("RecordInteger", {Scalar(0, "ProbeTop ")})});
    MapScriptInputs spacedInputs; spacedInputs.catalog = &*catalog.value; spacedInputs.triggers = &spaced;
    const auto top = GenerateMapScript(spacedInputs);
    Require(top.Succeeded() && top.text.find("call RecordInteger(1)") != std::string::npos,
        "preset names with surrounding spaces resolve to their value:\n" + top.text);

    // “获取对象handle值”(类型, 值) 没有同名脚本函数：按所选类型展开成整数转换，不能原样写出调用。
    const auto callValue = [](std::string function, std::vector<GraphArgumentValue> args) {
        GraphArgumentValue result;
        auto call = std::make_shared<GraphEcaValue>();
        call->function = std::move(function); call->kind = UiDefinitionKind::Call; call->arguments = std::move(args);
        result.call = call;
        return result;
    };
    const auto handleId = [&](const char* type, GraphArgumentValue value) {
        return Action("RecordInteger", {callValue("KKGetHandleId", {Scalar(0, type), std::move(value)})});
    };
    auto handles = MakeDocument(*catalog.value, {
        handleId("typename07_unit", callValue("ProbeUnit", {})),
        handleId("typename04_string", Scalar(3, "abc")),
        handleId("typename01_integer", Scalar(3, "5")),
    });
    MapScriptInputs handleInputs; handleInputs.catalog = &*catalog.value; handleInputs.triggers = &handles;
    const auto handle = GenerateMapScript(handleInputs);
    Require(handle.Succeeded() && !HasCode(handle.diagnostics, "kk-handle-type"), "handle id generation: " + Details(handle.diagnostics));
    Require(handle.text.find("call RecordInteger(GetHandleId(ProbeUnit()))") != std::string::npos
        && handle.text.find("call RecordInteger(StringHash(\"abc\"))") != std::string::npos
        && handle.text.find("call RecordInteger(5)") != std::string::npos
        && handle.text.find("KKGetHandleId") == std::string::npos,
        "handles use GetHandleId, strings StringHash, integers their own value:\n" + handle.text);
    auto unconvertible = MakeDocument(*catalog.value, {handleId("typename03_boolean", Scalar(3, "true"))});
    MapScriptInputs boolInputs; boolInputs.catalog = &*catalog.value; boolInputs.triggers = &unconvertible;
    const auto boolHandle = GenerateMapScript(boolInputs);
    Require(HasCode(boolHandle.diagnostics, "kk-handle-type"), "a boolean has no integer conversion and is reported: "
        + Details(boolHandle.diagnostics));
    // KKWE 对象表：主键、子键按各自类型转换为整数，值按第三个类型存取 YDHT。
    auto table = MakeDocument(*catalog.value, {
        Action("KKWESetObjectTableData", {Scalar(0, "typename07_unit"), callValue("ProbeUnit", {}),
            Scalar(0, "typename04_string"), Scalar(3, "hp"), Scalar(0, "typename01_integer"), Scalar(3, "5")}),
        Action("RecordInteger", {callValue("KKWEGetObjectTableData", {Scalar(0, "typename07_unit"), callValue("ProbeUnit", {}),
            Scalar(0, "typename04_string"), Scalar(3, "hp")})}),
        Action("KKWEFlushObjectTableData", {Scalar(0, "typename07_unit"), callValue("ProbeUnit", {}),
            Scalar(0, "typename04_string"), Scalar(3, "hp"), Scalar(0, "typename01_integer")}),
    });
    MapScriptInputs tableInputs; tableInputs.catalog = &*catalog.value; tableInputs.triggers = &table;
    const auto tableScript = GenerateMapScript(tableInputs);
    Require(tableScript.Succeeded() && !HasCode(tableScript.diagnostics, "storage-key-unsupported"),
        "object table generation: " + Details(tableScript.diagnostics));
    Require(tableScript.text.find("call SaveInteger(YDHT, GetHandleId(ProbeUnit()), StringHash(\"hp\"), 5)") != std::string::npos
        && tableScript.text.find("call RecordInteger(LoadInteger(YDHT, GetHandleId(ProbeUnit()), StringHash(\"hp\")))") != std::string::npos
        && tableScript.text.find("call RemoveSavedInteger(YDHT, GetHandleId(ProbeUnit()), StringHash(\"hp\"))") != std::string::npos
        && tableScript.text.find("ObjectTableData") == std::string::npos,
        "object table set/get/flush expand like KKWE:\n" + tableScript.text);

    // Dz 异步注册（多项）：只在指定玩家本机注册 ByCode 回调；回调读取的外层局部变量先拷进常量闭包页
    // （回调基础名的 StringHash），回调内从该页读取。
    GraphArgumentValue readBox;
    {
        auto call = std::make_shared<GraphEcaValue>();
        call->function = "YDWEGetAnyTypeLocalVariable"; call->kind = UiDefinitionKind::Call;
        call->arguments = {Scalar(0, "typename01_integer"), Scalar(3, "box")};
        readBox.call = call;
    }
    auto async = MakeDocument(*catalog.value, {
        Action("YDWESetAnyTypeLocalVariable", {Scalar(0, "typename01_integer"), Scalar(3, "box"), Scalar(3, "5")}),
        Action("DzFrameSetScriptMultiple", {Scalar(0, "Player00"), Scalar(3, "1"), Scalar(3, "7")},
            {Child(Action("RecordInteger", {readBox}))}),
        Action("DzTriggerRegisterKeyEventMultiple", {Scalar(0, "Player00"), Scalar(3, "1"), Scalar(3, "27")},
            {Child(Action("RecordInteger", {Scalar(3, "2")}))}),
    });
    MapScriptInputs asyncInputs; asyncInputs.catalog = &*catalog.value; asyncInputs.triggers = &async;
    const auto dz = GenerateMapScript(asyncInputs);
    Require(dz.Succeeded() && !HasCode(dz.diagnostics, "children-not-generated"), "Dz async generation: " + Details(dz.diagnostics));
    const auto dzBody = [&](std::string_view function) {
        const auto begin = dz.text.find("function " + std::string(function) + " takes");
        Require(begin != std::string::npos, "missing " + std::string(function) + ":\n" + dz.text);
        return dz.text.substr(begin, dz.text.find("endfunction", begin) - begin);
    };
    const auto frameCallback = dzBody("Trig_CallbackFunc002FT");
    const auto keyCallback = dzBody("Trig_CallbackFunc003KT");
    const auto dzMain = dzBody("Trig_CallbackActions");
    Require(frameCallback.find("call RecordInteger(LoadInteger(YDLOC, 0x088C955D, 0x") != std::string::npos,
        "the callback reads captured locals from its constant page: " + frameCallback);
    Require(keyCallback.find("call RecordInteger(2)") != std::string::npos, "key callback body: " + keyCallback);
    const auto copy = dzMain.find("call SaveInteger(YDLOC, 0x088C955D, 0x");
    const auto frameRegister = dzMain.find("call DzFrameSetScriptByCode(7, 1, function Trig_CallbackFunc002FT, false)");
    const auto keyRegister = dzMain.find("call DzTriggerRegisterKeyEventByCode(null, 27, 1, false, function Trig_CallbackFunc003KT)");
    Require(copy != std::string::npos && frameRegister != std::string::npos && keyRegister != std::string::npos && copy < frameRegister
        && Count(dzMain, "if GetLocalPlayer() == Player(0) then") == 2,
        "registration copies captured locals first and registers only for the chosen local player: " + dzMain);
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
